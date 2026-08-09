# Hands-on Learning

Hands-on Learning gives learners a local course workspace for reading, editing, running, and checking practical exercises.

## Language

**Course bundle**:
A complete, versioned course distribution that contains lessons, exercise files, checks, media, license terms, and attribution.
_Avoid_: Course archive, content package

**Normalized course**:
A course bundle that uses stable local IDs and paths and contains no source account data, source API details, or remote asset links.
_Avoid_: Scraped course, imported dump

**Raw source archive**:
Private staging material captured from an authorized course source before normalization.
_Avoid_: Course bundle, public course

**Course catalog**:
The public index of free course bundles that Hands-on Learning can install.
_Avoid_: Store, marketplace, registry

**Provider adapter**:
A private importer that transforms authorized source material into a normalized course.
_Avoid_: Scraper, downloader

**Learner workspace**:
The private local copy of exercise files that a learner can edit without changing the installed course bundle.
_Avoid_: Course files, source archive

**Check**:
The course-defined evaluation that decides whether an exercise or quiz is complete.
_Avoid_: Test when referring to learner completion

**Attribution**:
The public source and authorship notice required by a course permission or license.
_Avoid_: Ownership claim, permission record
