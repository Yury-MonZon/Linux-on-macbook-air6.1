DefinitionBlock ("", "SSDT", 2, "LOCAL", "MBREAD", 0x00000001)
{
    External (\_SB.PCI0.LPCB.EC.SMB0.SBRW, MethodObj)
    External (\_SB.PCI0.LPCB.EC.SMB0.SBRB, MethodObj)

    Method (\_SB.MBRD, 2, Serialized)
    {
        Local0 = 0
        If ((Arg0 == 0x00))
        {
            \_SB.PCI0.LPCB.EC.SMB0.SBRW (0x0B, Arg1, RefOf (Local0))
        }
        Else
        {
            \_SB.PCI0.LPCB.EC.SMB0.SBRB (0x0B, Arg1, RefOf (Local0))
        }
        Return (Local0)
    }
}
