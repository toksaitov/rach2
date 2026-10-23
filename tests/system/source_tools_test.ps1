# Execute the shared judge vectors as native children, with a system-only PATH.
param([string]$Prefix, [string]$Root, [string]$Output)
$ErrorActionPreference = 'Stop'

function Quote-Argument([string]$Value) {
    '"' + [regex]::Replace([regex]::Replace($Value, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1') + '"'
}

$results = @()
foreach ($entry in (Get-Content -Raw (Join-Path $Root 'plan.json') | ConvertFrom-Json)) {
    $start = New-Object System.Diagnostics.ProcessStartInfo
    $start.FileName = Join-Path $Prefix ('bin\' + $entry.tool + '.exe')
    $start.Arguments = (($entry.args | ForEach-Object { Quote-Argument $_ }) -join ' ')
    $start.WorkingDirectory = $Root
    $start.UseShellExecute = $false
    $start.RedirectStandardInput = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.EnvironmentVariables['PATH'] = (Join-Path $env:SystemRoot 'System32')
    $start.EnvironmentVariables['TZ'] = 'UTC'
    $start.EnvironmentVariables['LC_ALL'] = 'C'
    $child = New-Object System.Diagnostics.Process
    $child.StartInfo = $start
    $out = New-Object System.IO.MemoryStream
    $err = New-Object System.IO.MemoryStream
    $started = $false
    try {
        if (-not $child.Start()) { throw 'native judge did not start' }
        $started = $true
        $copyOut = $child.StandardOutput.BaseStream.CopyToAsync($out)
        $copyErr = $child.StandardError.BaseStream.CopyToAsync($err)
        if ($entry.input) {
            $inputBytes = [Convert]::FromBase64String($entry.input)
            $child.StandardInput.BaseStream.Write($inputBytes, 0, $inputBytes.Length)
        }
        $child.StandardInput.Close()
        $child.WaitForExit()
        [void]($copyOut.GetAwaiter().GetResult())
        [void]($copyErr.GetAwaiter().GetResult())
        $results += @{ id = $entry.id; status = $child.ExitCode;
            out = [Convert]::ToBase64String($out.ToArray()); err = [Convert]::ToBase64String($err.ToArray()) }
        Write-Host ($entry.id + ': native exit ' + $child.ExitCode)
    } finally {
        if ($started -and -not $child.HasExited) { $child.Kill(); $child.WaitForExit() }
        $child.Dispose()
        $out.Dispose()
        $err.Dispose()
    }
}
$json = ConvertTo-Json -Depth 8 -InputObject @($results)
[System.IO.File]::WriteAllText($Output, $json + "`n", (New-Object System.Text.UTF8Encoding $false))
