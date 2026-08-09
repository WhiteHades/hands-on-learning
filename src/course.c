#include "hol.h"

#include <archive.h>
#include <archive_entry.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libxml/HTMLparser.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char manifest_namespace[] =
  "http://www.imsglobal.org/xsd/imsccv1p3/imscp_v1p1";
static const char qti_namespace[] =
  "http://www.imsglobal.org/xsd/ims_qtiasiv1p2";
static const char lom_manifest_namespace[] =
  "http://ltsc.ieee.org/xsd/imsccv1p3/LOM/manifest";
static const char qti_assessment_type[] =
  "imsqti_xmlv1p2/imscc_xmlv1p3/assessment";
static const uint64_t maximum_expanded_bytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;

static int copy_string(char *target, size_t capacity, const char *value,
                       const char *field, hol_error *error) {
  size_t length = strlen(value);
  if (length >= capacity) {
    hol_error_set(error, HOL_ERR_SCHEMA, "%s exceeds %zu bytes", field,
                  capacity - 1U);
    return -1;
  }
  memcpy(target, value, length + 1U);
  return 0;
}

static int make_directories(const char *path, hol_error *error) {
  char copy[4096];
  if (copy_string(copy, sizeof(copy), path, "path", error) < 0) return -1;
  for (char *cursor = copy + 1; *cursor != '\0'; cursor++) {
    if (*cursor != '/') continue;
    *cursor = '\0';
    if (mkdir(copy, 0700) < 0 && errno != EEXIST) return -1;
    *cursor = '/';
  }
  return 0;
}

static int remove_tree(const char *path) {
  DIR *directory = opendir(path);
  if (directory == NULL) return errno == ENOENT ? 0 : -1;
  struct dirent *entry;
  while ((entry = readdir(directory)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
    char child[4096];
    int length = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
    if (length < 0 || (size_t)length >= sizeof(child)) {
      (void)closedir(directory);
      return -1;
    }
    struct stat status;
    if (lstat(child, &status) < 0) {
      (void)closedir(directory);
      return -1;
    }
    if (S_ISDIR(status.st_mode)) {
      if (remove_tree(child) < 0) {
        (void)closedir(directory);
        return -1;
      }
    } else if (unlink(child) < 0) {
      (void)closedir(directory);
      return -1;
    }
  }
  (void)closedir(directory);
  return rmdir(path);
}

static int extract_cartridge(const char *source, char root[4096], hol_error *error) {
  size_t source_length = strlen(source);
  if (source_length < 6U || strcmp(source + source_length - 6U, ".imscc") != 0) {
    hol_error_set(error, HOL_ERR_UNSUPPORTED, "course package must use the .imscc extension");
    return -1;
  }
  char canonical[4096];
  if (realpath(source, canonical) == NULL) {
    hol_error_set(error, HOL_ERR_PATH, "cannot resolve cartridge path");
    return -1;
  }
  (void)snprintf(root, 4096, "/tmp/hol-imscc-XXXXXX");
  if (mkdtemp(root) == NULL) return -1;
  struct archive *archive = archive_read_new();
  if (archive == NULL) goto failure;
  archive_read_support_format_zip(archive);
  if (archive_read_open_filename(archive, canonical, 64U * 1024U) != ARCHIVE_OK)
    goto archive_failure;
  size_t entries = 0U;
  uint64_t expanded = 0U;
  struct archive_entry *entry = NULL;
  int header_status = ARCHIVE_OK;
  while ((header_status = archive_read_next_header(archive, &entry)) == ARCHIVE_OK) {
    const char *archive_path = archive_entry_pathname(entry);
    char relative[HOL_PATH_MAX + 1];
    if (archive_path == NULL ||
        copy_string(relative, sizeof(relative), archive_path, "archive path", error) < 0)
      goto archive_failure;
    size_t relative_length = strlen(relative);
    while (relative_length > 0U && relative[relative_length - 1U] == '/')
      relative[--relative_length] = '\0';
    la_int64_t entry_size = archive_entry_size(entry);
    if (!hol_safe_relative_path(relative) ||
        archive_entry_symlink(entry) != NULL || archive_entry_hardlink(entry) != NULL ||
        entry_size < 0 || ++entries > 100000U ||
        (uint64_t)entry_size > maximum_expanded_bytes - expanded) goto archive_failure;
    expanded += (uint64_t)entry_size;
    char destination[4096];
    if (hol_join_path(destination, sizeof(destination), root, relative, error) < 0)
      goto archive_failure;
    mode_t type = archive_entry_filetype(entry);
    if (type == AE_IFDIR) {
      if (make_directories(destination, error) < 0 ||
          (mkdir(destination, 0700) < 0 && errno != EEXIST)) goto archive_failure;
      continue;
    }
    if (type != AE_IFREG || make_directories(destination, error) < 0)
      goto archive_failure;
    int output = open(destination, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (output < 0) goto archive_failure;
    char buffer[64U * 1024U];
    for (;;) {
      la_ssize_t count = archive_read_data(archive, buffer, sizeof(buffer));
      if (count < 0) {
        (void)close(output);
        goto archive_failure;
      }
      if (count == 0) break;
      size_t offset = 0U;
      while (offset < (size_t)count) {
        ssize_t written = write(output, buffer + offset, (size_t)count - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
          (void)close(output);
          goto archive_failure;
        }
        offset += (size_t)written;
      }
    }
    if (close(output) < 0) goto archive_failure;
  }
  if (header_status != ARCHIVE_EOF) goto archive_failure;
  int close_status = archive_read_close(archive);
  int free_status = archive_read_free(archive);
  if (close_status != ARCHIVE_OK || free_status != ARCHIVE_OK || entries == 0U)
    goto failure;
  char manifest[4096];
  if (hol_join_path(manifest, sizeof(manifest), root, "imsmanifest.xml", error) < 0 ||
      access(manifest, R_OK) < 0) goto failure;
  return 0;

archive_failure:
  (void)archive_read_close(archive);
  (void)archive_read_free(archive);
failure:
  (void)remove_tree(root);
  hol_error_set(error, HOL_ERR_SCHEMA, "unsafe or invalid Common Cartridge package");
  return -1;
}

static xmlNode *child(xmlNode *parent, const char *name) {
  for (xmlNode *node = parent != NULL ? parent->children : NULL;
       node != NULL; node = node->next)
    if (node->type == XML_ELEMENT_NODE && node->ns != NULL && parent->ns != NULL &&
        xmlStrEqual(node->ns->href, parent->ns->href) &&
        strcmp((const char *)node->name, name) == 0) return node;
  return NULL;
}

static xmlNode *descendant(xmlNode *parent, const char *name) {
  for (xmlNode *node = parent != NULL ? parent->children : NULL;
       node != NULL; node = node->next) {
    if (node->type != XML_ELEMENT_NODE || node->ns == NULL || parent->ns == NULL ||
        !xmlStrEqual(node->ns->href, parent->ns->href)) continue;
    if (strcmp((const char *)node->name, name) == 0) return node;
    xmlNode *nested = descendant(node, name);
    if (nested != NULL) return nested;
  }
  return NULL;
}

static xmlNode *child_namespace(xmlNode *parent, const char *name,
                                const char *namespace) {
  for (xmlNode *node = parent != NULL ? parent->children : NULL;
       node != NULL; node = node->next)
    if (node->type == XML_ELEMENT_NODE && node->ns != NULL &&
        strcmp((const char *)node->name, name) == 0 &&
        strcmp((const char *)node->ns->href, namespace) == 0) return node;
  return NULL;
}

static bool bounded_regular_file(const char *path, uint64_t maximum) {
  struct stat status;
  return lstat(path, &status) == 0 && S_ISREG(status.st_mode) &&
         status.st_nlink == 1 && status.st_size >= 0 &&
         (uint64_t)status.st_size <= maximum;
}

static int node_text(xmlNode *node, char *target, size_t capacity,
                     const char *field, hol_error *error) {
  if (node == NULL) return -1;
  xmlChar *value = xmlNodeGetContent(node);
  if (value == NULL) return -1;
  int result = copy_string(target, capacity, (const char *)value, field, error);
  xmlFree(value);
  return result;
}

static int attribute(xmlNode *node, const char *name, char *target, size_t capacity,
                     hol_error *error) {
  xmlChar *value = node != NULL ? xmlGetProp(node, (const xmlChar *)name) : NULL;
  if (value == NULL) return -1;
  int result = copy_string(target, capacity, (const char *)value, name, error);
  xmlFree(value);
  return result;
}

static xmlNode *resource_by_id(xmlNode *resources, const char *identifier) {
  for (xmlNode *node = resources != NULL ? resources->children : NULL;
       node != NULL; node = node->next) {
    if (node->type != XML_ELEMENT_NODE ||
        strcmp((const char *)node->name, "resource") != 0) continue;
    xmlChar *value = xmlGetProp(node, (const xmlChar *)"identifier");
    bool matches = value != NULL && strcmp((const char *)value, identifier) == 0;
    xmlFree(value);
    if (matches) return node;
  }
  return NULL;
}

static int write_reader_text(const hol_course *course, const char *id,
                             const char *text, char relative[HOL_PATH_MAX + 1],
                             hol_error *error) {
  int length = snprintf(relative, HOL_PATH_MAX + 1U, ".reader/%s.txt", id);
  if (length < 0 || length > HOL_PATH_MAX) return -1;
  char path[4096];
  if (hol_join_path(path, sizeof(path), course->root, relative, error) < 0) return -1;
  return hol_atomic_write(path, text, strlen(text), error);
}

static int parse_web_content(hol_course *course, hol_lesson *lesson,
                             xmlNode *resource, hol_error *error) {
  char href[HOL_PATH_MAX + 1];
  if (attribute(resource, "href", href, sizeof(href), error) < 0 ||
      !hol_safe_relative_path(href)) return -1;
  char path[4096];
  if (hol_join_path(path, sizeof(path), course->root, href, error) < 0) return -1;
  xmlNode *file = child(resource, "file");
  char declared[HOL_PATH_MAX + 1];
  if (attribute(file, "href", declared, sizeof(declared), error) < 0 ||
      strcmp(declared, href) != 0 || !bounded_regular_file(path, 16U * 1024U * 1024U))
    return -1;
  xmlDoc *document = htmlReadFile(path, NULL, HTML_PARSE_NONET | HTML_PARSE_NOERROR |
                                              HTML_PARSE_NOWARNING);
  if (document == NULL) return -1;
  xmlChar *content = xmlNodeGetContent(xmlDocGetRootElement(document));
  if (content == NULL) {
    xmlFreeDoc(document);
    return -1;
  }
  lesson->kind = HOL_LESSON_READING;
  int result = write_reader_text(course, lesson->id, (const char *)content,
                                 lesson->content_path, error);
  xmlFree(content);
  xmlFreeDoc(document);
  return result;
}

static int parse_qti_item(xmlNode *item, hol_quiz_question *question,
                          hol_error *error) {
  xmlNode *metadata = descendant(item, "qtimetadata");
  bool profile_found = false;
  for (xmlNode *field = metadata != NULL ? metadata->children : NULL;
       field != NULL; field = field->next) {
    if (field->type != XML_ELEMENT_NODE ||
        strcmp((const char *)field->name, "qtimetadatafield") != 0) continue;
    char label[64];
    char entry[64];
    if (node_text(child(field, "fieldlabel"), label, sizeof(label), "profile", error) == 0 &&
        node_text(child(field, "fieldentry"), entry, sizeof(entry), "profile", error) == 0 &&
        strcmp(label, "cc_profile") == 0 &&
        strcmp(entry, "cc.multiple_choice.v0p1") == 0) profile_found = true;
  }
  if (attribute(item, "ident", question->id, sizeof(question->id), error) < 0 ||
      !hol_valid_id(question->id) || !profile_found ||
      node_text(descendant(descendant(item, "presentation"), "mattext"),
                question->prompt, sizeof(question->prompt), "prompt", error) < 0)
    return -1;
  xmlNode *render = descendant(item, "render_choice");
  xmlNode *response = descendant(item, "response_lid");
  char cardinality[16];
  if (attribute(response, "rcardinality", cardinality, sizeof(cardinality), error) < 0 ||
      strcmp(cardinality, "Single") != 0) return -1;
  size_t count = 0U;
  for (xmlNode *node = render != NULL ? render->children : NULL;
       node != NULL; node = node->next)
    if (node->type == XML_ELEMENT_NODE &&
        strcmp((const char *)node->name, "response_label") == 0) count++;
  if (count < 2U || count > 10U) return -1;
  question->choices = calloc(count, sizeof(*question->choices));
  if (question->choices == NULL) return -1;
  question->choice_count = count;
  size_t index = 0U;
  for (xmlNode *node = render->children; node != NULL; node = node->next) {
    if (node->type != XML_ELEMENT_NODE ||
        strcmp((const char *)node->name, "response_label") != 0) continue;
    if (attribute(node, "ident", question->choices[index].id,
                  sizeof(question->choices[index].id), error) < 0 ||
        node_text(descendant(node, "mattext"), question->choices[index].text,
                  sizeof(question->choices[index].text), "choice", error) < 0)
      return -1;
    for (size_t previous = 0U; previous < index; previous++)
      if (strcmp(question->choices[previous].id, question->choices[index].id) == 0)
        return -1;
    index++;
  }
  if (node_text(descendant(item, "varequal"), question->answer,
                sizeof(question->answer), "answer", error) < 0) return -1;
  bool answer_found = false;
  for (size_t choice = 0U; choice < question->choice_count; choice++)
    if (strcmp(question->choices[choice].id, question->answer) == 0) answer_found = true;
  if (!answer_found) return -1;
  xmlNode *feedback = descendant(item, "itemfeedback");
  if (feedback != NULL)
    (void)node_text(descendant(feedback, "mattext"), question->explanation,
                    sizeof(question->explanation), "feedback", error);
  return 0;
}

static size_t find_qti_passing_score(xmlNode *node, size_t maximum) {
  for (xmlNode *current = node != NULL ? node->children : NULL;
       current != NULL; current = current->next) {
    if (current->type != XML_ELEMENT_NODE) continue;
    if (strcmp((const char *)current->name, "qtimetadatafield") == 0) {
      xmlChar *label = xmlNodeGetContent(child(current, "fieldlabel"));
      xmlChar *entry = xmlNodeGetContent(child(current, "fieldentry"));
      if (label != NULL && entry != NULL &&
          strcmp((const char *)label, "qmd_masteryscore") == 0) {
        char *end = NULL;
        unsigned long value = strtoul((const char *)entry, &end, 10);
        xmlFree(label);
        xmlFree(entry);
        if (end != NULL && *end == '\0' && value >= 1UL && value <= maximum)
          return (size_t)value;
        return 0U;
      }
      xmlFree(label);
      xmlFree(entry);
    }
    size_t nested = find_qti_passing_score(current, maximum);
    if (nested > 0U) return nested;
  }
  return 0U;
}

static int parse_qti(hol_course *course, hol_lesson *lesson, xmlNode *resource,
                     hol_error *error) {
  xmlNode *file = child(resource, "file");
  char href[HOL_PATH_MAX + 1];
  if (attribute(file, "href", href, sizeof(href), error) < 0 ||
      !hol_safe_relative_path(href)) return -1;
  char path[4096];
  if (hol_join_path(path, sizeof(path), course->root, href, error) < 0) return -1;
  if (file->next != NULL || !bounded_regular_file(path, 4U * 1024U * 1024U)) return -1;
  xmlDoc *document = xmlReadFile(path, NULL, XML_PARSE_NONET | XML_PARSE_NOBLANKS);
  if (document == NULL) return -1;
  xmlNode *root = xmlDocGetRootElement(document);
  if (root == NULL || root->ns == NULL ||
      strcmp((const char *)root->ns->href, qti_namespace) != 0 ||
      strcmp((const char *)root->name, "questestinterop") != 0) goto failure;
  xmlNode *assessment = child(root, "assessment");
  bool assessment_profile = false;
  xmlNode *assessment_metadata = child(assessment, "qtimetadata");
  for (xmlNode *field = assessment_metadata != NULL ? assessment_metadata->children : NULL;
       field != NULL; field = field->next) {
    if (field->type != XML_ELEMENT_NODE ||
        strcmp((const char *)field->name, "qtimetadatafield") != 0) continue;
    char label[64];
    char entry[64];
    if (node_text(child(field, "fieldlabel"), label, sizeof(label), "profile", error) == 0 &&
        node_text(child(field, "fieldentry"), entry, sizeof(entry), "profile", error) == 0 &&
        strcmp(label, "cc_profile") == 0 && strcmp(entry, "cc.exam.v0p1") == 0)
      assessment_profile = true;
  }
  xmlNode *section = child(assessment, "section");
  size_t count = 0U;
  for (xmlNode *node = section != NULL ? section->children : NULL;
       node != NULL; node = node->next)
    if (node->type == XML_ELEMENT_NODE && strcmp((const char *)node->name, "item") == 0)
      count++;
  if (!assessment_profile || count == 0U || count > 100U) goto failure;
  lesson->questions = calloc(count, sizeof(*lesson->questions));
  if (lesson->questions == NULL) goto failure;
  lesson->question_count = count;
  size_t passing_score = find_qti_passing_score(assessment, count);
  lesson->quiz_passing_score = passing_score > 0U ? passing_score : count;
  size_t index = 0U;
  for (xmlNode *node = section->children; node != NULL; node = node->next) {
    if (node->type != XML_ELEMENT_NODE || strcmp((const char *)node->name, "item") != 0)
      continue;
    if (parse_qti_item(node, &lesson->questions[index++], error) < 0) goto failure;
  }
  lesson->kind = HOL_LESSON_QUIZ;
  char summary[HOL_TEXT_MAX + 64U];
  (void)snprintf(summary, sizeof(summary), "%s\n\nThis assessment contains %zu questions.\n",
                 lesson->title, count);
  if (write_reader_text(course, lesson->id, summary, lesson->content_path, error) < 0)
    goto failure;
  xmlFreeDoc(document);
  return 0;

failure:
  xmlFreeDoc(document);
  return -1;
}

static int parse_lesson(hol_course *course, hol_lesson *lesson, xmlNode *item,
                        xmlNode *resources, hol_error *error) {
  char reference[HOL_ID_MAX + 1];
  if (attribute(item, "identifier", lesson->id, sizeof(lesson->id), error) < 0 ||
      !hol_valid_id(lesson->id) ||
      attribute(item, "identifierref", reference, sizeof(reference), error) < 0 ||
      node_text(child(item, "title"), lesson->title, sizeof(lesson->title),
                "lesson title", error) < 0) return -1;
  xmlNode *resource = resource_by_id(resources, reference);
  char type[128];
  if (resource == NULL || attribute(resource, "type", type, sizeof(type), error) < 0)
    return -1;
  if (strcmp(type, "webcontent") == 0)
    return parse_web_content(course, lesson, resource, error);
  if (strcmp(type, qti_assessment_type) == 0)
    return parse_qti(course, lesson, resource, error);
  hol_error_set(error, HOL_ERR_UNSUPPORTED, "unsupported Common Cartridge resource: %s", type);
  return -1;
}

static int parse_organization(hol_course *course, xmlNode *organizations,
                              xmlNode *resources, hol_error *error) {
  xmlNode *organization = child(organizations, "organization");
  xmlNode *root_item = child(organization, "item");
  char structure[32];
  if (organization == NULL || root_item == NULL ||
      attribute(organization, "structure", structure, sizeof(structure), error) < 0 ||
      strcmp(structure, "rooted-hierarchy") != 0) return -1;
  size_t chapter_count = 0U;
  for (xmlNode *node = root_item->children; node != NULL; node = node->next)
    if (node->type == XML_ELEMENT_NODE && strcmp((const char *)node->name, "item") == 0)
      chapter_count++;
  if (chapter_count == 0U || chapter_count > 256U) return -1;
  course->chapters = calloc(chapter_count, sizeof(*course->chapters));
  if (course->chapters == NULL) return -1;
  course->chapter_count = chapter_count;
  size_t chapter_index = 0U;
  for (xmlNode *node = root_item->children; node != NULL; node = node->next) {
    if (node->type != XML_ELEMENT_NODE || strcmp((const char *)node->name, "item") != 0)
      continue;
    hol_chapter *chapter = &course->chapters[chapter_index++];
    if (attribute(node, "identifier", chapter->id, sizeof(chapter->id), error) < 0 ||
        !hol_valid_id(chapter->id) ||
        node_text(child(node, "title"), chapter->title, sizeof(chapter->title),
                  "chapter title", error) < 0) return -1;
    size_t lesson_count = 0U;
    for (xmlNode *lesson = node->children; lesson != NULL; lesson = lesson->next)
      if (lesson->type == XML_ELEMENT_NODE &&
          strcmp((const char *)lesson->name, "item") == 0) lesson_count++;
    if (lesson_count == 0U || course->lesson_count > 10000U - lesson_count) return -1;
    chapter->lessons = calloc(lesson_count, sizeof(*chapter->lessons));
    if (chapter->lessons == NULL) return -1;
    chapter->lesson_count = lesson_count;
    size_t lesson_index = 0U;
    for (xmlNode *lesson = node->children; lesson != NULL; lesson = lesson->next) {
      if (lesson->type != XML_ELEMENT_NODE ||
          strcmp((const char *)lesson->name, "item") != 0) continue;
      if (parse_lesson(course, &chapter->lessons[lesson_index++], lesson,
                       resources, error) < 0) return -1;
    }
    course->lesson_count += lesson_count;
  }
  return 0;
}

int hol_course_load(const char *source, hol_course **output, hol_error *error) {
  if (source == NULL || output == NULL) return -1;
  *output = NULL;
  hol_course *course = calloc(1U, sizeof(*course));
  if (course == NULL) return -1;
  if (realpath(source, course->source_path) == NULL ||
      extract_cartridge(source, course->root, error) < 0) goto failure;
  course->owns_root = true;
  char manifest_path[4096];
  if (hol_join_path(manifest_path, sizeof(manifest_path), course->root,
                    "imsmanifest.xml", error) < 0) goto failure;
  if (!bounded_regular_file(manifest_path, 4U * 1024U * 1024U)) goto failure;
  xmlDoc *document = xmlReadFile(manifest_path, NULL, XML_PARSE_NONET | XML_PARSE_NOBLANKS);
  if (document == NULL) goto failure;
  xmlNode *manifest = xmlDocGetRootElement(document);
  if (manifest == NULL || manifest->ns == NULL ||
      strcmp((const char *)manifest->name, "manifest") != 0 ||
      strcmp((const char *)manifest->ns->href, manifest_namespace) != 0) goto xml_failure;
  xmlNode *metadata = child(manifest, "metadata");
  char schema[64];
  char schema_version[32];
  if (node_text(child(metadata, "schema"), schema, sizeof(schema), "schema", error) < 0 ||
      node_text(child(metadata, "schemaversion"), schema_version,
                sizeof(schema_version), "schema version", error) < 0 ||
      strcmp(schema, "IMS Common Cartridge") != 0 ||
      strcmp(schema_version, "1.3.0") != 0) goto xml_failure;
  xmlNode *lom = child_namespace(metadata, "lom", lom_manifest_namespace);
  xmlNode *general = child(lom, "general");
  xmlNode *lifecycle = child(lom, "lifeCycle");
  xmlNode *rights = child(lom, "rights");
  if (node_text(child(child(general, "identifier"), "entry"), course->id,
                sizeof(course->id), "course id", error) < 0 ||
      !hol_valid_id(course->id) ||
      node_text(child(child(general, "title"), "string"), course->title,
                sizeof(course->title), "title", error) < 0 ||
      node_text(child(child(general, "description"), "string"), course->description,
                sizeof(course->description), "description", error) < 0 ||
      node_text(child(child(lifecycle, "version"), "string"), course->version,
                sizeof(course->version), "version", error) < 0 ||
      node_text(child(child(rights, "description"), "string"), course->attribution,
                sizeof(course->attribution), "rights", error) < 0) goto xml_failure;
  course->schema_version = 1U;
  (void)snprintf(course->minimum_app_version, sizeof(course->minimum_app_version), "%s",
                 HOL_APP_VERSION);
  const char *spdx = strstr(course->attribution, "SPDX-License-Identifier:");
  if (spdx != NULL) {
    spdx += strlen("SPDX-License-Identifier:");
    while (*spdx == ' ') spdx++;
    size_t length = strcspn(spdx, "\r\n");
    if (length >= sizeof(course->license_spdx)) goto xml_failure;
    memcpy(course->license_spdx, spdx, length);
    course->license_spdx[length] = '\0';
  } else (void)snprintf(course->license_spdx, sizeof(course->license_spdx), "NOASSERTION");
  (void)snprintf(course->license_file, sizeof(course->license_file), "LICENSE");
  if (parse_organization(course, child(manifest, "organizations"),
                         child(manifest, "resources"), error) < 0) goto xml_failure;
  xmlFreeDoc(document);
  *output = course;
  return 0;

xml_failure:
  xmlFreeDoc(document);
failure:
  hol_course_free(course);
  if (error != NULL && error->message[0] == '\0')
    hol_error_set(error, HOL_ERR_SCHEMA, "invalid Common Cartridge package");
  return -1;
}

const hol_lesson *hol_course_lesson(const hol_course *course, size_t index) {
  if (course == NULL) return NULL;
  for (size_t chapter = 0U; chapter < course->chapter_count; chapter++) {
    if (index < course->chapters[chapter].lesson_count)
      return &course->chapters[chapter].lessons[index];
    index -= course->chapters[chapter].lesson_count;
  }
  return NULL;
}

void hol_course_free(hol_course *course) {
  if (course == NULL) return;
  for (size_t chapter = 0U; chapter < course->chapter_count; chapter++) {
    for (size_t lesson_index = 0U;
         lesson_index < course->chapters[chapter].lesson_count; lesson_index++) {
      hol_lesson *lesson = &course->chapters[chapter].lessons[lesson_index];
      free(lesson->files);
      free(lesson->runner.expected_output);
      for (size_t question = 0U; question < lesson->question_count; question++)
        free(lesson->questions[question].choices);
      free(lesson->questions);
      for (size_t media = 0U; media < lesson->media_count; media++)
        free(lesson->media_paths[media]);
      free(lesson->media_paths);
    }
    free(course->chapters[chapter].lessons);
  }
  free(course->chapters);
  if (course->owns_root) (void)remove_tree(course->root);
  free(course);
}
