
## v0.22 — module Block singletons (namespacing fix)

- **parser:** `mod.Block.method(args)`, `mod.Block.field` (read/write) now
  parse in both expression and statement positions, emitting
  MEMBER_CALL/ACCESS/ASSIGN with the mangled owner (`mod__Block`).
  Previously: "expected '(' or '=' after module member name".
- **parser:** inside a module, references to Blocks declared in the same
  module (`Vault.bump(x)` from a module fn or method) now mangle the owner
  via the existing module_decls table. Previously: undefined identifier.
- Tests: tests/modules/modules.sh cases c22a–c22e + fixtures/static/vault.flx.
- No runtime/resolver/VM changes — parser-only; full suite green.

## graph: BACKSPACE / TAB key names

- graph_key_code now maps "BACKSPACE" → KEY_BACKSPACE and "TAB" → KEY_TAB
  (raylib backend). Previously these strings returned 0, so
  key_pressed(win, "BACKSPACE") always reported false — text-entry backspace
  could never fire. ("F" already resolved via the single-letter A-Z rule;
  the F-key crash some callers saw was an OLD binary predating graph.fullscreen,
  not a key-mapping gap.)
- No behavior change for the stub backend (headless, no key events).

## graph: proportional fullscreen (render-to-texture scaling)

- The raylib backend now renders each frame into an offscreen RenderTexture at
  the logical (design) resolution passed to graph.init, then blits it to the
  real window scaled to fit and centered, with black letterbox/pillarbox bars.
  Previously fullscreen just enlarged the window and the game stayed at its
  original size in the top-left corner with the rest painted in the clear color.
- graph.begin_frame draws into the target (BeginTextureMode); graph.end_frame
  finishes it and does the scaled DrawTexturePro blit. graph.close unloads the
  texture.
- graph.mouse_x / mouse_y now un-project window coordinates back into logical
  space (accounting for the letterboxed scale), so mouse input still lines up
  with what the game drew.
- Stub backend unchanged (headless). NOTE: the raylib path can only be compiled
  with raylib present; verify on a FLUXA_GRAPH_RAYLIB=1 build.
