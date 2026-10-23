# SF5K3Q — OCQ2 product-region ownership gate

**Status: portable focused host test PASS with ASan/UBSan on a local
source checkout (legacy magic constants matched), full ORUN host regression
and RAK4630 target build NOT RUN. No physical device update.**

The SF5K3P candidate currently uses the single reserved 128 KiB product
partition for new PRODUCT_SECURE 84/100-byte durable custody (OCQ2/v3). Its
physical FlashBackend is shared with the older 73-byte OCQ1/v2 custody store
and the OBS1 observation store, but their on-flash layouts are NOT
interchangeable.

This slice adds an explicit `Census::product_custody` counter, a new
`mountProductCustodyDecision` for OCQ2, and invokes both the census and
the strict `ProductCustodyStore::scanAtBoot` before allowing admission.
Legacy `mountDecision(Owner::kCustody)` remains the OCQ1 decision;
OBS1 and OCQ1 may not mount OCQ2. The existing tracker ObservationStore
also rejects OCQ2 as foreign. An OCQ1/OCQ2 or OBS1/OCQ2 mixture is
non-mountable; unknown pages are never silently erased or interpreted
as valid. In a mixed OCQ2/unknown case the strict record scanner rejects
the actual flash content.

Focused regression exercises an erased region, each magic, mixed layouts,
unknown marker and read failure, plus existing fake-NOR custody reboot/
async/failure tests. The new script is added to full host tests.
No flash data are erased or migrated.

Remaining production gates: full target build, physical flash qualification,
provisioned per Tracker/Gateway/grant scoped ACK key and admission security,
live RF/Edge composition, and Tracker verified ACK release. Existing
`BASE` is not automatically a Gateway.
