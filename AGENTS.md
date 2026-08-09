# UI typography contract

- All repository documentation, comments, and user-facing UI copy must be written in English.
- RmlUI documents must use the semantic typography classes defined in `client/ui/typography.rcss`.
- Do not add `font-family`, `font-size`, `font-weight`, `font-style`, `line-height`, `letter-spacing`, or `text-transform` to component or page stylesheets. Typography belongs in `typography.rcss`.
- Use at most two font families across the application. `Inter` is the primary UI family; a second family may only be introduced for a clearly distinct role such as code or cryptographic material.
- Prefer semantic roles (`ui-heading-page`, `ui-heading`, `ui-body`, `ui-control`, `ui-label`, `ui-caption`, `ui-micro`, `ui-symbol`) over one-off text styling. Weight utilities are reserved for exceptional emphasis and must not replace the semantic role.
- When modifying legacy markup, migrate the touched text to semantic typography classes and remove duplicated typography declarations from the component stylesheet.
