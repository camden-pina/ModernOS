/* Optional destructive-liveness fixture for a disposable guest only.
 * Never include in normal firmware or an OS image. This SSDT intentionally
 * leaves synchronous S5 preparation blocked forever in a firmware Event wait.
 * There are no operation regions, hardware accesses, or terminal power writes.
 *
 * The event is never signaled. Wait timeout 0xFFFF reaches the port's
 * ACPI_WAIT_FOREVER semaphore path. The OS must report preparation as pending
 * or blocked; a harness timeout is not successful cancellation or rollback.
 * Dispose of the guest from the host after recording that evidence.
 *
 * Root _PTS is a standard firmware method: if the base DSDT already supplies
 * it, loading this SSDT can fail with a duplicate-name error. Do not override
 * the original method or credit such a rejected table as a blocked-prep test.
 */
DefinitionBlock ("", "SSDT", 2, "MODOS ", "BLKPREP ", 1)
{
    Event (BPWE)

    Method (_PTS, 1, NotSerialized)
    {
        If (Arg0 == 5)
        {
            Wait (BPWE, 0xFFFF)
        }
    }
}
