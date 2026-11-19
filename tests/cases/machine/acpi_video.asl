/*
 * acpi_video.asl: the SSDT that the acpi suite's video case and the
 * machine.screen.levels case give QEMU with -acpitable (../machine.acpi.json,
 * ../machine.screen.json): a video bus with one output, as a laptop's
 * graphics device has, its panel's brightness, and methods that
 * /proc/acpi/call fires.
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

        /* The internal panel, as _DOD names it, with a brightness the
         * kernel's acpi_video0 drives as Linux's acpi_video does: _BCL's
         * levels after its first two, 0 to 100 by 5, _BCM to set one and
         * _BQC to read it back. BCLV is the level, at 100 from power on. */
        Device (LCD0)
        {
            Name (_ADR, 0x0400)
            Name (BCLV, 0x64)

            Method (_BCL, 0, NotSerialized)
            {
                Return (Package (0x17)
                {
                    0x64, 0x32,
                    0x00, 0x05, 0x0A, 0x0F, 0x14, 0x19, 0x1E, 0x23, 0x28, 0x2D, 0x32,
                    0x37, 0x3C, 0x41, 0x46, 0x4B, 0x50, 0x55, 0x5A, 0x5F, 0x64
                })
            }

            Method (_BCM, 1, NotSerialized)
            {
                BCLV = Arg0
            }

            Method (_BQC, 0, NotSerialized)
            {
                Return (BCLV)
            }
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
