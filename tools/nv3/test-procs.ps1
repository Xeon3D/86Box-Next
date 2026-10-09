param([switch]$Kill)
# The private test VM: 86Box-Next.exe running from $env:NV3_WORK\rig (NV3_WORK as a POSIX path is fine)
$rig = (& cygpath -w "$env:NV3_WORK") + '\rig\*'
$p = Get-CimInstance Win32_Process -Filter "Name='86Box-Next.exe'" | Where-Object { $_.ExecutablePath -like $rig }
if ($Kill) { $p | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }; Start-Sleep -Milliseconds 800 }
else { @($p).Count }
