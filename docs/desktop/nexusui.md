# NexusUI component reference

`import org.nexus.ui` gives every NEXUS application the design system. Standard
Qt Quick Controls (`Button`, `TextField`, `Switch`, `ComboBox`, `Menu`,
`Dialog`, `TabBar`, `ScrollBar`, …) are already styled by `org.nexus.style`;
the components below add the composite pieces. Sources: `desktop/ui/qml/`,
live reference: `nexus-ui-gallery` (`desktop/ui/gallery/`).

## Singletons

| Name | Purpose |
|---|---|
| `Theme` | Design tokens (generated). Colours: `bgBase bgSurface bgRaised bgOverlay bgInput bgHover bgPressed borderSubtle border borderStrong textPrimary textSecondary textMuted textDisabled textOnAccent success warning danger accent accentFill accentViolet accentBlue …`. Type: `fontUi fontMono fontSizeCaption/Small/Body/BodyLarge/Title/Heading/Display/Hero weightRegular/Medium/Semibold/Bold`. Layout: `spaceXxs…spaceXxxl radiusXs…radiusPill icon iconSmall iconLarge controlHeight rowHeight headerHeight sidebarWidth`. Motion: `motionFast/Normal/Slow`. Helpers: `alpha(c,a) mix(a,b,t) formatBytes(n) formatRate(n) formatDuration(s) accentByName(name)`. |
| `Appearance` | Live appearance state shared by all processes: `dark`, `accent`, `reduceMotion`, `transparency`, `fontScale` (read/write). |

## Components

| Component | Key API |
|---|---|
| `NxWindow` | `ApplicationWindow` with token sizes and `toaster` (`toaster.show(text, "info"|"success"|"warning"|"danger")`). |
| `NxPage` | Scrollable page: `title`, `subtitle`, `actions: [..]`, `maxContentWidth`; children go into a column (`Layout.fillWidth`). |
| `NxSidebar` | `title`, `model: [{key,title,icon,section,badge}]`, `currentKey`, `onActivated(key)`, `header`, `footer`, `collapsed`. |
| `NxSection` | Titled card of rows: `title`, `description`, `headerActions`; children are `NxRow`s or any `Layout` item. |
| `NxRow` | `icon`, `iconColor`, `title`, `subtitle`, `value`, `monoValue`, `chevron`, `clickable`, `onClicked`; children are trailing controls. |
| `NxCard` | Raised surface: `padding`, `hoverable`, `accentBorder`; children fill the padded area. |
| `NxStatTile` | Dashboard figure: `label`, `value`, `detail`, `icon`, `tone` (`neutral|accent|success|warning|danger`), `loading`. |
| `NxGauge` | Arc gauge: `value` (0–1), `text`, `label`, `color`, `available` (false → "n/a"). |
| `NxSparkline` | History chart: `values` (array, oldest first), `maxValue` (≤0 = auto), `capacity`, `color`, `fill`. |
| `NxMeter` | Labelled usage bar: `label`, `valueText`, `value` (0–1), `toned`. |
| `NxDataTable` | `columns: [{title, role, width|fill, align, mono, format(v,row), color(v,row), sortable}]`, `model` (item model or JS array), `sortRole`, `sortAscending`, `currentRow`, signals `sortRequested(role, asc)`, `rowSelected`, `rowActivated`, `contextMenuRequested`. Sorting is done by the model owner. |
| `NxButton` | `kind` (`primary|secondary|ghost|danger`), `iconName`, `busy`. |
| `NxIconButton` | `iconName`, `tooltip` (required), `iconSize`, `checkable`. |
| `NxSearchField`, `NxPasswordField` | `TextField`s with search glyph/clear button and reveal toggle. |
| `NxSegmented` | `model: [..]`, `currentIndex`, `onActivated(index)`. |
| `NxBadge` | `text`, `kind` (`neutral|accent|success|warning|danger`), `dot`. |
| `NxBanner` | Inline message: `kind` (`info|success|warning|danger`), `title`, `message`, `actionText`, `dismissible`, `onActionTriggered`. |
| `NxEmptyState` | `icon`, `title`, `message`, `actionText`, `tone`, `onActionTriggered` — for empty lists, unavailable services, errors. |
| `NxConfirmDialog` | `title`, `message`, `confirmText`, `destructive`, `requiredPhrase`, `onConfirmed`. |
| `NxKeyValue` | `label`, `value` (selectable), `mono`, `labelWidth`. |
| `NxToaster` | In-window toasts (already inside `NxWindow`). |
| `NxDivider` | Hairline; `vertical`. |
| `NxIcon` | Symbolic glyph: `name`, `size`, `color`. Names: `desktop/ui/icons/*.svg` (regenerate with `assets/src/icons/gen-symbolic.py`; never hand-edit the SVGs). |
| `NxAppIcon` | Themed application/file icon: `name` (icon-theme name or absolute path), `size`. |

## Conventions

* A list/table/page has three explicit states: loading, populated, and
  empty/error (`NxEmptyState`).
* Layouts: children of `NxPage`/`NxSection` use `Layout.*` properties, not
  anchors.
* Delegates declare `required property` for the model roles they use.
* Icons in `Button`/`ToolButton`/`MenuItem`: `icon.source: "image://nxicon/<name>"`
  (tinted by the style) — or `NxButton.iconName`.
* A missing symbolic icon renders as a red crossed box and logs a warning, so
  it is caught by the smoke test; add the glyph to the generator instead of
  substituting an unrelated one.
