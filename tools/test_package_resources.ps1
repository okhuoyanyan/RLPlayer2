param([string]$WorkDir='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'package_resources.ps1')
if(-not $WorkDir){$WorkDir=Join-Path $env:TEMP ('rlplayer-license-controls-'+[guid]::NewGuid().ToString('N'))}
New-Item -ItemType Directory -Path $WorkDir | Out-Null
$results=@()
foreach($case in @('valid','missing-license','missing-opra','missing-third-party','empty-license','empty-opra','empty-third-party')){
 $root=Join-Path $WorkDir $case;$src=Join-Path $root 'source';$out=Join-Path $root 'out'
 New-Item -ItemType Directory -Path $src,$out | Out-Null
 [IO.File]::WriteAllBytes((Join-Path $src 'LICENSE'),[byte[]](65,0,10,255))
 [IO.File]::WriteAllBytes((Join-Path $src 'OPRA-NOTICE.txt'),[byte[]](239,187,191,79,80,82,65,13,10))
 [IO.File]::WriteAllBytes((Join-Path $src 'THIRD-PARTY-NOTICES.txt'),[byte[]](78,79,84,73,67,69,10))
 if($case -eq 'missing-license'){Remove-Item -LiteralPath (Join-Path $src 'LICENSE')}
 if($case -eq 'missing-opra'){Remove-Item -LiteralPath (Join-Path $src 'OPRA-NOTICE.txt')}
 if($case -eq 'missing-third-party'){Remove-Item -LiteralPath (Join-Path $src 'THIRD-PARTY-NOTICES.txt')}
 if($case -eq 'empty-license'){[IO.File]::WriteAllBytes((Join-Path $src 'LICENSE'),[byte[]]@())}
 if($case -eq 'empty-opra'){[IO.File]::WriteAllBytes((Join-Path $src 'OPRA-NOTICE.txt'),[byte[]]@())}
 if($case -eq 'empty-third-party'){[IO.File]::WriteAllBytes((Join-Path $src 'THIRD-PARTY-NOTICES.txt'),[byte[]]@())}
 $ok=$false;$detail=''
 try{$copied=@(Copy-RequiredReleaseNotices -SourceRoot $src -OutputDirectory $out);$ok=$true}catch{$detail=$_.Exception.Message}
 $expected=$case -eq 'valid';$correct=$ok -eq $expected
 if($ok){foreach($name in @('LICENSE','OPRA-NOTICE.txt','THIRD-PARTY-NOTICES.txt')){$correct=$correct -and ((Get-FileHash -LiteralPath (Join-Path $src $name)).Hash -eq (Get-FileHash -LiteralPath (Join-Path $out $name)).Hash)}}
 else{$correct=$correct -and @(Get-ChildItem -LiteralPath $out).Count -eq 0}
 $results+=[pscustomobject]@{case=$case;correct=$correct;accepted=$ok;detail=$detail}
 Write-Output "$case correct=$correct accepted=$ok"
}
$results|ConvertTo-Json -Depth 4|Out-File -LiteralPath (Join-Path $WorkDir 'regression.json') -Encoding utf8
if(@($results|Where-Object {-not $_.correct}).Count){throw 'Required license resource regression failed'}
Write-Output 'PASS 7/7 required license copy controls; original bytes preserved'
