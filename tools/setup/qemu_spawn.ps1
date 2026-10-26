# Complete the native QEMU package's GLib command-spawning dependency.

# Return the selected native runtime, using the package default off PATH.
function Get-QemuDirectory {
 $program=Get-Command qemu-system-x86_64.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
 if($program){return Split-Path -Parent $program.Source}
 return Join-Path $env:ProgramFiles 'qemu'
}

# Require the command helpers from the selected GLib's own package.
function Test-QemuSpawn {
 param([Parameter(Mandatory=$true)][string]$QemuDirectory)
 $packages=@{
  '7b5c19f5540857ec2288323f3b1654cbe5655fc3ea21417a25da69bece489eec'=@(
   '87113f2bf18d694910a1e6791282aea4da3611d41a8397434e43fed6deec72a5',
   'c8734364a23a2af11660efba3d4c9b82bcdc604f15816cfd92ed2697ee30cdaf');
  '3a1abec0184b238db54399cb9243393d9436d96b8fdb9bd9f2875da207c5b458'=@(
   'c95201fa81637687c1a5bd70d68f438f2a7c2aab79e261f9781e5b424cb210d6',
   '38113f6dedf7a367bcf629abdc256ffb33e5d2a2f1a05476580303cfeb03a533')
 }
 try{
  $library=Get-FileHash -LiteralPath (Join-Path $QemuDirectory 'libglib-2.0-0.dll') -Algorithm SHA256 -ErrorAction Stop
  $helpers=$packages[$library.Hash.ToLowerInvariant()]
  if(!$helpers){return $false}
  $names=@('gspawn-win64-helper.exe','gspawn-win64-helper-console.exe')
  for($i=0;$i -lt $names.Count;$i++){
   $path=Join-Path $QemuDirectory $names[$i]
   if(!(Test-Path -LiteralPath $path -PathType Leaf) -or
      (Get-FileHash -LiteralPath $path -Algorithm SHA256 -ErrorAction Stop).Hash.ToLowerInvariant() -cne $helpers[$i]){
    return $false
   }
  }
  return $true
 }catch{return $false}
}

function Install-QemuSpawn {
 param([Parameter(Mandatory=$true)][string]$QemuDirectory,
       [string]$CacheDirectory=([IO.Path]::GetTempPath()))
 $ErrorActionPreference='Stop'
 $root=[IO.Path]::GetFullPath($QemuDirectory)
 $library=Join-Path $root 'libglib-2.0-0.dll'
 $expectedLibrary='7b5c19f5540857ec2288323f3b1654cbe5655fc3ea21417a25da69bece489eec'
 $package='mingw-w64-x86_64-glib2-2.88.3-1-any.pkg.tar.zst'
 $packageSha='5aa850934d0216f5b7237bd54c3ec660ebfc3a42fd992bc7abbd611bd9cc7663'
 $uri='https://repo.msys2.org/mingw/mingw64/'+$package
 $helpers=@{
  'gspawn-win64-helper.exe'='87113f2bf18d694910a1e6791282aea4da3611d41a8397434e43fed6deec72a5'
  'gspawn-win64-helper-console.exe'='c8734364a23a2af11660efba3d4c9b82bcdc604f15816cfd92ed2697ee30cdaf'
 }
 if(!(Test-Path -LiteralPath $library -PathType Leaf) -or
    (Get-FileHash -LiteralPath $library -Algorithm SHA256).Hash.ToLowerInvariant() -cne $expectedLibrary){
  throw 'QEMU GLib has no matching pinned command helpers'
 }
 foreach($name in $helpers.Keys){
  $path=Join-Path $root $name
  if((Test-Path -LiteralPath $path) -and
     (!(Test-Path -LiteralPath $path -PathType Leaf) -or
      (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $helpers[$name])){
   throw ('existing QEMU helper differs: '+$path)
  }
 }
 $cache=[IO.Path]::GetFullPath($CacheDirectory)
 $null=[IO.Directory]::CreateDirectory($cache)
 $scratch=Join-Path $cache ('rach2-qemu-spawn.'+[Guid]::NewGuid().ToString('N'))
 $null=[IO.Directory]::CreateDirectory($scratch)
 try{
  $archive=Join-Path $scratch $package
  Invoke-WebRequest -UseBasicParsing -Uri $uri -OutFile $archive
  if((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -cne $packageSha){
   throw 'QEMU helper package digest differs'
  }
  $tar='C:/msys64/usr/bin/tar.exe'
  $cygpath='C:/msys64/usr/bin/cygpath.exe'
  $archivePosix=(& $cygpath -u $archive).Trim()
  if($LASTEXITCODE -ne 0 -or !$archivePosix){throw 'QEMU archive path conversion failed'}
  $scratchPosix=(& $cygpath -u $scratch).Trim()
  if($LASTEXITCODE -ne 0 -or !$scratchPosix){throw 'QEMU scratch path conversion failed'}
  & $tar --force-local -xf $archivePosix -C $scratchPosix `
    mingw64/bin/libglib-2.0-0.dll mingw64/bin/gspawn-win64-helper.exe `
    mingw64/bin/gspawn-win64-helper-console.exe
  if($LASTEXITCODE -ne 0){throw 'QEMU helper extraction failed'}
  $stage=Join-Path $scratch 'mingw64/bin'
  $pins=@{}+$helpers
  $pins['libglib-2.0-0.dll']=$expectedLibrary
  foreach($name in $pins.Keys){
   $path=Join-Path $stage $name
   $item=Get-Item -LiteralPath $path
   if($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
      (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $pins[$name]){
    throw ('extracted QEMU input differs: '+$name)
   }
  }
  if((Get-FileHash -LiteralPath $library -Algorithm SHA256).Hash.ToLowerInvariant() -cne $expectedLibrary){
   throw 'QEMU GLib changed during helper preparation'
  }
  $copied=@();$observed=@{}
  foreach($name in $helpers.Keys){
   $path=Join-Path $root $name
   if(!(Test-Path -LiteralPath $path)){
    [IO.File]::Copy((Join-Path $stage $name),$path,$false)
    $copied+=,$name
   }
   if((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $helpers[$name]){
    throw ('installed QEMU helper differs: '+$path)
   }
   $observed[$name]=@{path=$path;sha256=$helpers[$name];size_bytes=(Get-Item -LiteralPath $path).Length}
  }
  $record=@{format='rach2-qemu-spawn-install';schema_version=1;
   package=@{url=$uri;sha256=$packageSha;version='2.88.3-1'};
   library=@{path=$library;sha256=$expectedLibrary};helpers=$observed;
   copied=$copied;observed_utc=[DateTime]::UtcNow.ToString('o')}
  $record|ConvertTo-Json -Depth 6
 }finally{
  Remove-Item -LiteralPath $scratch -Recurse -Force
 }
}
