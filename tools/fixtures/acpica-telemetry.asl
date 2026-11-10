/* Optional test SSDT. Load explicitly in a disposable guest; never part of
 * normal firmware or the OS image. No operation regions or hardware writes.
 * The host parser test does not claim to execute this AML fixture.
 */
DefinitionBlock ("", "SSDT", 2, "MODOS ", "PWRTELM ", 1)
{
    Scope (\_SB)
    {
        Device (TAC0)
        {
            Name (_HID, "ACPI0003")
            Name (_UID, 0x544100)
            Name (_STA, 0x0F)
            Name (_PSR, One)
        }
        Device (TBT0)
        {
            Name (_HID, EisaId ("PNP0C0A"))
            Name (_UID, 0x544200)
            Name (_STA, 0x1F)
            Name (BIXD, Package (21)
            {
                One, Zero, 6000, 5000, One, 12000, 400, 200,
                20, 99000, 1000, 100, 2000, 1000, One, One,
                "Telemetry mWh", "Fixture0", "Li-ion", "Coast test", Zero
            })
            Method (_BIX, 0, NotSerialized) { Return (BIXD) }
            Name (NSEQ, Zero)
            Name (RMNC, 2500)
            Method (_BST, 0, Serialized)
            {
                Local0 = Package (4) {One, 1000, 2500, 12000}
                Local0 [2] = RMNC
                If (NSEQ == Zero)
                {
                    /* Exactly two genuine deferred notifications per boot.
                     * Return the first 50% reading, then let the queued
                     * notifications drive a new cached 60% sample after the
                     * driver's one-second coalescing interval. Only AML-owned
                     * namespace objects change; there is no hardware access.
                     */
                    NSEQ = One
                    RMNC = 3000
                    Notify (\_SB.TBT0, 0x80)
                    Notify (\_SB.TBT0, 0x81)
                }
                Return (Local0)
            }
        }
        Device (TBT1)
        {
            Name (_HID, "PNP0C0A")
            Name (_UID, 0x544201)
            Name (_STA, 0x1F)
            /* Missing _BIX exercises the standard _BIF fallback, in mAh. */
            Name (_BIF, Package (13)
            {
                One, 6000, 5000, One, 12000, 400, 200, One, One,
                "Telemetry mAh", "Fixture1", "Li-ion", "Coast test"
            })
            Name (_BST, Package (4) {0x0A, 0xFFFFFFFF, 4000, 0xFFFFFFFF})
        }
        Device (TBT2)
        {
            Name (_HID, "PNP0C0A")
            Name (_UID, 0x544202)
            /* The bay exists, but bit 4 says no battery: do not read data. */
            Name (_STA, 0x0F)
        }
        Device (TBT3)
        {
            Name (_HID, "PNP0C0A")
            Name (_UID, 0x544203)
            Name (_STA, 0x1F)
            Name (BIXD, Package (20)
            {
                Zero, Zero, 0xFFFFFFFF, 0xFFFFFFFF, One, 0xFFFFFFFF, 400, 200,
                0xFFFFFFFF, 99000, 0xFFFFFFFF, 0xFFFFFFFF, 2000, 1000, One, One,
                "Telemetry unknown", "Fixture3", "Li-ion", "Coast test"
            })
            Method (_BIX, 0, NotSerialized) { Return (BIXD) }
            Name (_BST, Package (4) {One, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF})
        }
    }
}
