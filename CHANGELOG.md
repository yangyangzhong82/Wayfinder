# Changelog

All notable changes to this project will be documented in this file.

## Unreleased

- Preserve continuous stair/ramp trails across cave slices and surface transitions; retrace across layers with up/down guidance. Save v2 trails while preserving legacy v1 breaks.
- Add per-world waypoint group visibility for each map, favorite-first sorting, and multi-select batch regrouping. Keep navigation targets visible and hidden markers out of hit testing.
- Use a native Windows Unicode editor with IME composition, cursor selection and copy/cut/paste. Accept X/Z or X/Y/Z coordinate tuples transactionally and preserve explicit heights when creating located waypoints.
- Add cross-layer persistence/retrace and interaction regression coverage, including native input cancellation and shutdown.

- Strengthen summit/ridge and cliff-lip/foot separation with bounded local convexity and hollow shading, including symmetric peaks and cliffs facing across the light. Preserve near-cliff gradients when broad samples stop at a drop, with headroom for material detail and night readability.
- Test all cliff orientations, minimap filtering, night lighting and reopened history; stabilise half-channel rounding so live and historical overview colours agree regardless of tile iteration order. Add a monochrome cliff/peak preview.

- Improve terrain separation with four-direction, multi-scale slope shading: combine immediate steps with connected four-block hillside gradients, add subtle hollow occlusion and outline real ledges/shorelines on all four sides. Stop broad gradients at sheer drops, water and unexplored cells to avoid roof halos.
- Keep live and historical relief consistent across all chunk edges, single-tile archive eviction and distant summaries without changing the history format; retain readable texture highlights and shadows.

- Render per-block top-face textures at close zoom, with shared immutable 16x16 snapshots, alpha-weighted mip levels and smooth transitions to overview colours. Apply biome tint, terrain relief and day/night lighting to individual texels.
- Persist material palettes, cell references, tint, rotation and block-state hashes in v7 history; retain v1-v6 compatibility and stable fallback while asynchronous textures load. Identical resident materials are shared; old history gains textures on resampling.
- Resolve position-selected texture regions, source-image versus atlas UVs, mirrored UVs, horizontal pillar textures and cardinal rotation. Keep liquids as map colours and composite transparent holes over terrain colour; animations and full block-model rendering are not included.
- Add material regression tests and a synthetic before/after preview covering exact texels, mip filtering, tint/rotation, negative/world-limit coordinates, archive reload, palette deduplication, corrupt records and v6 migration.

- Give terrain softer northwest relief with bounded highlights/shadows and remove artificial eight-block contour bands. Preserve block colour boundaries up to one block per texture pixel, retaining area filtering for distant views.
- Apply biome tint to the actual vegetation top-face representative colour so grass and leaves retain material brightness; retain safe biome-colour fallback while textures load.
- Cover vegetation tint, continuous slopes, cliff contrast, shoreline relief and fractional zoom/history parity, with an optional synthetic terrain preview.

- Sample an 8x8 representative average from the current resource pack's top-face UV for non-biome-tinted solid blocks, with asynchronous image lookup, one new material per Tick, and the existing map color as fallback. Keep biome-tinted foliage, water and lava colors intact.

- Refresh the player's nearby 5x5 chunk area on two of every three regular sampling turns while retaining the full-area sweep and immediate changed-block priority. Unchanged rescans no longer dirty history or trigger image revisions.
- Reuse each sampled position's liquid-block lookup, reducing repeated client block queries within the Tick budget.

- Fix surface lava pools being drawn as their solid bottom: inspect and climb liquids above the engine solid-height hint before the downward terrain scan, with bounded work and missing-data checks.
- Lift night shadows and midtones so terrain materials and relief remain readable while preserving daylight and full-bright light sources.

- Add an enabled-by-default Night and lighting switch for both maps, using sampled sky/block light and the Overworld sky darkening value; retain readable terrain and bright light sources at night and in caves.
- Persist light levels in v6 history with v1–v5 compatibility, refresh stationary maps on lighting changes, reject stale background lighting results and retain lit summaries for distant overviews.
- Cover light falloff, settings persistence, v5 migration, invalid light records and live/archive lighting parity across zoom levels and negative chunk seams.

- Increase biome region fill opacity from 20% to 40% to make colored areas easier to distinguish while retaining terrain detail.

- Add a persistent Biome regions switch for both maps, with subtle region tints, known-to-known boundary lines and localized connected-region labels.
- Save per-column biome palettes in v5 map history, retain v1–v4 terrain compatibility and support historical cursor biome lookups across dimensions and cave layers; old regions acquire biome records when resampled.
- Cover palette recycling, partial updates, migration/corruption, world/layer isolation, negative-coordinate seams, unknown frontiers, live/archive raster parity and settings persistence.

- Show the biome under the fullscreen map cursor using loaded terrain at the displayed surface/cave height; clear stale hover names on block, layer, dimension and session changes.
- Add a persistent Slime chunks overlay switch for both maps: seed-independent Bedrock chunk calculation, translucent green Overworld cells, zoom/work limits and negative-coordinate regression coverage.

- Preserve block color boundaries at close zoom and add narrow, zoom-faded relief/shoreline edges from sampled heights and water; retain area filtering for overview and biome colors for interiors.
- Cover fractional and negative coordinates, world-limit seams, unknown neighbors, live/history parity and optional synthetic before/after terrain previews.

- Extend minimap and fullscreen zoom-in limits fourfold (0.125 blocks per logical pixel; minimap up to 16x).
- Give Quality a persistent 4x terrain raster density, doubling its previous texture dimensions without changing world coverage; upgrade existing Quality configs and retain trail width across density changes.

- Double terrain raster dimensions in both map modes while preserving world coverage, zoom behavior and trail width; keep UV cropping and asynchronous history acceptance consistent with the denser textures.
- Cover one-block terrain detail, live/history parity, sub-block scrolling, world-boundary UVs and trail alignment with regression checks.

- Add height-aware navigation and arrival checks; fade ordinary waypoints from other cave slices while keeping the navigation target bright.
- Add bounded, world-isolated exploration trails with background atomic saves, recording/display controls, confirmed clearing and reverse guidance along a continuous trail segment.
- Composite trails into terrain textures with clipped, bounded rasterization rather than per-point GUI submissions.
- Add an Explore toolbar panel for browsing and locking saved dimension/height layers independently of live sampling; keep map picking, fit-all, waypoints and historical height requests on the displayed layer.
- Hide live entities/player markers on other layers, disable cross-dimension map teleport, and restore the live layer with Home/Follow or on reopening the map.
- Add exploration regressions for vertical arrival, cave marker opacity, recording gaps, persistence, retracing, raster clipping, layer locking and small-window controls.

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
