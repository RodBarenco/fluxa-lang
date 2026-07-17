
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
