# Fleet protocol schemas (server channel, v1)

Copy of the normative JSON Schemas from the platform repo (docs/FLEET.md D18):
`Auto-Tournament/auto-tournament`, `api/src/integrations/cs2/fleet/protocol/v1/**/*.json`,
branch `feat/fleet-step1` at commit 4649814 (PR #386). Do not edit here; copy them again when the
platform changes them:

    cp -r <auto-tournament>/api/src/integrations/cs2/fleet/protocol/v1/{envelope,defs}.json \
          <auto-tournament>/api/src/integrations/cs2/fleet/protocol/v1/{messages,http} plugins/fleet/protocol/v1/

`fleet_integration_test` (ctest `fleet_integration`) validates every frame fleet.so sends and every
enrollment body against them with `tests/schema_check.cpp`, so a copy that no longer matches what
fleet.so produces fails CI.
