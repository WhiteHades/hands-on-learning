# Hands-on Learning

Hands-on Learning is a local terminal reader for portable digital learning content.

## Language

**Common Cartridge**:
A 1EdTech Common Cartridge 1.3 package with the `.imscc` extension, a root `imsmanifest.xml`, HTML resources, LOM metadata, and QTI assessments.
_Avoid_: HOL course, proprietary bundle, custom course format

**Cartridge source**:
The unpacked standards-compliant files used to build a Common Cartridge package.
_Avoid_: Custom manifest, normalized bundle

**Raw source archive**:
Private staging material captured from an authorized course source before standards conversion.
_Avoid_: Common Cartridge, public course

**Course catalog**:
The public JSON index that records cartridge and optional exercise profile URLs, sizes, digests, compatibility, and display metadata.
_Avoid_: Store, marketplace, course format

**Exercise profile**:
A strict declarative JSON companion that binds application-owned local exercise behavior to one exact Common Cartridge, lesson item, referenced resource, and set of file digests.
_Avoid_: Course format, executable manifest, shell recipe

**Sandbox stage**:
A fresh private directory assembled for one Run or Check action from learner-editable workspace files and verified readonly or hidden cartridge resources.
_Avoid_: Learner workspace, package extraction, persistent build directory

**Provider adapter**:
A private importer that transforms authorized source material into HTML, LOM, QTI, and Common Cartridge structures.
_Avoid_: Scraper, public loader

**Learner progress**:
Private local completion state keyed by stable cartridge and item identifiers.
_Avoid_: Cartridge content, source archive

**Attribution**:
The public source and authorship notice represented in LOM rights metadata and catalog output.
_Avoid_: Ownership claim, permission record
