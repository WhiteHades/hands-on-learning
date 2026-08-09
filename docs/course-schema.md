# Supported Course Standards

Hands-on Learning reads 1EdTech Common Cartridge 1.3 packages. A package uses the `.imscc` extension and ZIP format. It must contain `imsmanifest.xml` at the archive root.

The application does not define a private course manifest, package extension, or assessment format.

## Standards

The reader uses these published standards:

- [1EdTech Common Cartridge 1.3](https://www.imsglobal.org/cc/ccv1p3/imscc_Implementation-v1p3.html)
- [1EdTech Content Packaging 1.2](https://www.1edtech.org/standards/content-packaging)
- [IEEE Learning Object Metadata](https://standards.ieee.org/ieee/1484.12.1/7699/)
- [Common Cartridge QTI 1.2.1 profile](https://www.imsglobal.org/profile/cc/ccv1p3/ccv1p3_qtiasiv1p2p1_v1p0.xsd)

## Package Requirements

The manifest uses this namespace:

```text
http://www.imsglobal.org/xsd/imsccv1p3/imscp_v1p1
```

Manifest metadata must contain:

```xml
<schema>IMS Common Cartridge</schema>
<schemaversion>1.3.0</schemaversion>
```

The package must contain one rooted organization. Folder items become chapters. Learner items reference resources through `identifierref`.

Hands-on Learning supports these Common Cartridge resource types:

- `webcontent`
- `imsqti_xmlv1p2/imscc_xmlv1p3/assessment`

The reader reports unsupported resource types instead of interpreting them as custom content.

## Web Content

Web lessons use the standard `webcontent` resource type. The resource `href` must name a packaged HTML file and must also appear in a child `file` element.

The terminal reader extracts readable text from HTML. Other Common Cartridge readers can present the same HTML directly.

## Assessments

Assessments use the Common Cartridge profile of QTI 1.2.1 and this namespace:

```text
http://www.imsglobal.org/xsd/ims_qtiasiv1p2
```

The current reader supports the required single response multiple choice profile:

```text
cc.multiple_choice.v0p1
```

Each assessment contains one section. Each item contains a prompt, response choices, a correct `varequal` response, and optional item feedback.

## Integrity And Safety

The catalog records each complete package byte count and SHA-256 digest. The importer also rejects:

- absolute archive paths
- `.` and `..` path segments
- symbolic links and hard links
- special files
- duplicate extracted paths
- more than 100,000 entries
- more than 2 GiB of expanded data
- XML documents that request network access

## Native Code Exercises

Common Cartridge does not standardize editable terminal workspaces, compiler profiles, SQL runners, shell commands, or expected process output. Hands-on Learning does not hide those fields in a private XML extension.

Starter files can remain ordinary Common Cartridge files. Other readers can download them. Automatic compilation and checking require a separate published interoperability standard and are outside the current cartridge profile.

## Source Imports

Private provider adapters may transform authorized source material into HTML, QTI, LOM, and Common Cartridge packages. Public packages contain no source credentials, account state, source API details, or remote asset URLs.
