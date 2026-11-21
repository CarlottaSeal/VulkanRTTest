# Fetches ONNX Runtime (CPU, win-x64) headers and libs into ThirdParty/onnxruntime.
param([string]$Version = "1.32.0")

$dst = Join-Path $PSScriptRoot "onnxruntime"
$tmp = Join-Path $env:TEMP "ort-$Version"
New-Item -ItemType Directory -Force $tmp | Out-Null
Invoke-WebRequest "https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime/$Version/microsoft.ml.onnxruntime.$Version.nupkg" -OutFile "$tmp\ort.zip"
Expand-Archive "$tmp\ort.zip" "$tmp\x" -Force

New-Item -ItemType Directory -Force "$dst\include", "$dst\lib" | Out-Null
Copy-Item "$tmp\x\build\native\include\*" "$dst\include" -Recurse -Force
Copy-Item "$tmp\x\runtimes\win-x64\native\onnxruntime*.dll", "$tmp\x\runtimes\win-x64\native\onnxruntime.lib" "$dst\lib" -Force
Copy-Item "$tmp\x\LICENSE" $dst
