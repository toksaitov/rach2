/*
 * acpi_video.asl: the SSDT that the acpi suite's video case gives QEMU with
 * -acpitable (../machine.acpi.json): a video bus with one output, as a
 * laptop's graphics device has, and methods that /proc/acpi/call fires.
 * acpi_video.aml beside it is this, compiled by the pinned ACPICA's iasl:
 *
 *     iasl -p acpi_video acpi_video.asl
 */
DefinitionBlock ("", "SSDT", 2, "RACH2", "RVIDTEST", 0x00000001)
{
    /* A video bus, by its _DOS and _DOD, which the kernel finds as Linux's
     * acpi_video.c does. DOSV keeps the last argument of _DOS. */
    Device (\_SB.RVID)
    {
        Name (_HID, "RACH0001")
        Name (DOSV, 0xFF)

        Method (_DOS, 1, NotSerialized)
        {
            DOSV = Arg0
        }

        Method (_DOD, 0, NotSerialized)
        {
            Return (Package (0x01) { 0x80010400 })
        }

        /* The internal panel, as _DOD names it. */
        Device (LCD0)
        {
            Name (_ADR, 0x0400)
        }
    }

    /* The brightness keys, down and then up, as the Lenovo's _Q11 and _Q12
     * notify its panel. */
    Method (\RTST, 0, NotSerialized)
    {
        Notify (\_SB.RVID.LCD0, 0x87)
        Notify (\_SB.RVID.LCD0, 0x86)
    }

    /* What the kernel last gave _DOS. */
    Method (\RDOS, 0, NotSerialized)
    {
        Return (\_SB.RVID.DOSV)
    }

    /* Two integers and a string as arguments, for the parser of the calls. */
    Method (\RADD, 2, NotSerialized)
    {
        Return ((Arg0 + Arg1))
    }

    Method (\RCAT, 1, NotSerialized)
    {
        Return (Concatenate (Arg0, "cd"))
    }
}
