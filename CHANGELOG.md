# Changelog

All notable changes to this project will be documented in this file.

## Unreleased

- Remove black backdrops from mob portraits, dots, count labels and player names; make the minimap panel, scale bar and navigation caption backgrounds transparent.

- Isolate every mob portrait's image submission to prevent mixed-species UI batches from sharing texture state after a report of skeleton-only heads.
- Cluster nearby same-species mobs with accurate count badges, preserve distinct species and height groups, fan crowded icons out with position leaders and keep player names separate.
- Reduce portrait backdrops and accept the built-in Mob type bit alongside runtime categories while retaining non-living exclusions.
- Add mixed-crowd, count conservation, zoom separation, stable placement and texture-submission regression checks with an optional synthetic preview export.

- Add persistent minimap entity icon scale (default 75%) and relative-height fading (default 65% transparency for entities at least four blocks below the player), with in-game controls.
- Apply lower opacity consistently to portraits, fallback dots, backgrounds and player names, using separate image batches; keep fullscreen icon sizes unchanged.
- Limit radar snapshots, counts and observed types to living mobs and players; exclude items, vehicles, projectiles and non-living Mob subclasses, including obsolete non-living selections.

- Add default-on portraits for 74 vanilla mob types using client resource-pack textures, with a persistent portrait/dot switch.
- Preserve player names, entity filtering and fallback markers; account for portrait bounds when placing names.
- Normalize entity tint/emissive alpha masks in private cached textures so sheep faces and glowing eyes remain visible in the UI.
- Bound portrait texture conversion to one new texture per frame, support texture reloads and add UV/pixel/configuration regressions.

- Add nearby entity markers to both maps, player names, persistent toggles/radius and searchable entity-type selections.
- Collect client runtime actors and players directly, deduplicate the snapshot and remove the spatial-query/isInWorld gate after a report of missing markers.
- Show loaded/nearby/matched counts in entity settings and add entity filter, configuration, projection and player-name regressions.

- Add a configurable Overworld automatic cave-switch height (default Y=48), with in-game one-block controls, persistence and a four-block return margin.

- Add automatic Overworld cave maps, height-sliced Nether terrain and End island/void maps to both map views.
- Add persistent automatic/surface/cave settings, explicit layer labels and liquid-surface sampling.
- Isolate cave history by dimension and eight-block height slice; write v4 tiles while reading v1/v2/v3 surface history.
- Keep terrain lookup, fit-all bounds and teleport refresh on the selected layer; reject wall/void destinations and stale background results.
- Add terrain regressions covering cave floors, lava/water, missing chunks, layer isolation, archive eviction and legacy formats.

- Disable forced teleport destination block checks to address rejected teleports into unloaded areas; retain surface-height and permission validation.
- Add a right-click location menu with permission-checked teleport requests, retained waypoint creation and asynchronous historical height lookup.
- Strengthen terrain relief with northwest slope lighting, altitude bands, elevation contours and continuous water-depth shading.
- Cover teleport coordinates/menu bounds and live-versus-history terrain shading in feature regressions.
- Load and migrate map history in the background with visible progress, preserving live samples.
- Isolate damaged tile files individually while retaining healthy history and corruption backups.
- Add waypoint name search, dimension/group filters, distance/name sorting and persisted group labels.
- Click map markers to edit, Shift-click to navigate, and avoid overlapping map labels.
- Add persistent minimap zoom hotkeys/settings, water/leaves toggles, performance presets and confirmed reset.
- Add standalone regression build targets for history, waypoint UI, settings and existing persistence/atlas tests.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
