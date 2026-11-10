/* Opt-in QEMU-only SSDT. Every writable operation region receives a physical
 * page owned by the guest harness. Firmware NVS is accessed read-only. */
DefinitionBlock ("", "SSDT", 2, "MODERN", "ACPITEST", 0x00000001)
{
    Scope (\_SB)
    {
        Device (MTST)
        {
            Name (_HID, "MDS0001")
            Name (_UID, One)
            Mutex (MTX0, 0)
            Name (CNTR, Zero)

            /* This address is an unassigned, read-only q35 MMIO hole in the
             * disposable fixture VM (no IOMMU/TPM device is attached). Never
             * use this SSDT on physical hardware. The Field is first touched
             * after scheduling, so BSP preflight must already have mapped it. */
            OperationRegion (LMMI, SystemMemory, 0xFED90000, 4)
            Field (LMMI, DWordAcc, NoLock, Preserve) { LVAL, 32 }
            Method (LMMR, 0, Serialized) { Return (LVAL) }

            Method (ARTH, 2, NotSerialized)
            {
                If (Acquire (MTX0, 0xFFFF)) { Return (Ones) }
                Multiply (Arg0, 7, Local0)
                Xor (Arg1, 0x55, Local1)
                Add (Local0, Local1, Local0)
                Release (MTX0)
                Return (Local0)
            }

            Method (PACK, 0, NotSerialized)
            {
                Return (Package (4)
                {
                    0x12345678,
                    "ACPICA",
                    Buffer (4) { 0x00, 0x55, 0xAA, 0xFF },
                    Package (2) { 0x42, 0xC0DEC0DE }
                })
            }

            Method (MEMT, 2, Serialized)
            {
                OperationRegion (MRAM, SystemMemory, Arg0, 8)
                Field (MRAM, DWordAcc, NoLock, Preserve)
                {
                    M000, 32,
                    M004, 32
                }
                Store (Arg1, M000)
                Xor (Arg1, 0xA5A55A5A, M004)
                Store (Package (2) { Zero, Zero }, Local0)
                Store (M000, Index (Local0, Zero))
                Store (M004, Index (Local0, One))
                Return (Local0)
            }

            Method (NRED, 1, Serialized)
            {
                OperationRegion (NVSR, SystemMemory, Arg0, One)
                Field (NVSR, ByteAcc, NoLock, Preserve) { NV00, 8 }
                ToInteger (NV00, Local0)
                Return (Local0)
            }

            Method (SLPT, 0, NotSerialized)
            {
                If (Acquire (MTX0, 0xFFFF)) { Return (Ones) }
                Store (CNTR, Local0)
                Sleep (5)
                Stall (50)
                Increment (Local0)
                Store (Local0, CNTR)
                Release (MTX0)
                Return (Local0)
            }

            Method (TRIG, 0, NotSerialized)
            {
                Notify (MTST, 0x80)
            }
        }
    }

    /* q35 has GPEs 00..3F; 0F is unused by its built-in DSDT. Registration
     * and runtime enablement are tested without injecting hardware side effects. */
    Scope (\_GPE)
    {
        Name (GCNT, Zero)
        Method (_E0F, 0, NotSerialized) { Increment (GCNT) }
    }

}
