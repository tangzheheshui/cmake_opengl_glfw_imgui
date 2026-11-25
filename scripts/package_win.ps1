# ============================================================
# package_win.ps1 —— Windows 免安装包（F2 Windows 版）
#
# 用法（在仓库根目录）：
#   powershell -ExecutionPolicy Bypass -File scripts/package_win.ps1
#   powershell -ExecutionPolicy Bypass -File scripts/package_win.ps1 -Config Release -BuildDir build/win-msvc
#
# 产物：dist/MyDuck-win/（可整目录拷走）+ dist/MyDuck-win.zip
#   启动：双击 my_app.exe（默认 OpenGL）；Vulkan 用 run_vulkan.bat。
#
# 依赖说明：
#   - GLFW/Assimp/FreeType 都是静态链接，不需要额外 DLL；
#   - Vulkan 需要 vulkan-1.dll：优先从本机 System32 拷一份进包，
#     目标机没装 Vulkan 运行库时也能起（没有可用 GPU 会自动回退 OpenGL）；
#   - MSVC 运行库（vcruntime140/msvcp140）不打包：目标机需装
#     VC++ 2015-2022 Redistributable（README 里写明）。
# ============================================================
param(
    [string]$Config = "Debug",
    [string]$BuildDir = "build/win-verify",
    [string]$AppName = "MyDuck"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root

$exe = Join-Path $root "$BuildDir/app/$Config/my_app.exe"
if (-not (Test-Path $exe)) {
    Write-Error "找不到 $exe，请先构建：cmake --build $BuildDir --config $Config"
}

$dist = Join-Path $root "dist/$AppName-win"
if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
New-Item -ItemType Directory -Path $dist | Out-Null

Copy-Item $exe (Join-Path $dist "my_app.exe")
Copy-Item -Recurse (Join-Path $root "res") (Join-Path $dist "res")

# Vulkan loader：从 System32 带一份，目标机没装运行库也能起
$vkDll = Join-Path $env:WINDIR "System32/vulkan-1.dll"
if (Test-Path $vkDll) {
    Copy-Item $vkDll (Join-Path $dist "vulkan-1.dll")
} else {
    Write-Warning "System32 里没有 vulkan-1.dll，包内不含 Vulkan 运行库（目标机需自行安装）"
}

@'
@echo off
set RHI_BACKEND=vulkan
start "" "%~dp0my_app.exe"
'@ | Set-Content -Encoding ASCII (Join-Path $dist "run_vulkan.bat")

@'
@echo off
set RHI_BACKEND=opengl
start "" "%~dp0my_app.exe"
'@ | Set-Content -Encoding ASCII (Join-Path $dist "run_opengl.bat")

@"
$AppName （Windows 免安装包）

运行：
  双击 my_app.exe          默认 OpenGL
  run_vulkan.bat           使用 Vulkan（失败会自动回退 OpenGL）

要求：
  - 显卡驱动支持 OpenGL 3.3；Vulkan 需要驱动提供 Vulkan 1.0+ 运行库
  - 需要 VC++ 2015-2022 Redistributable（vcruntime140/msvcp140）

可选环境变量：
  RHI_BACKEND=opengl|vulkan   选后端
  RHI_VK_VALIDATION=0         关掉 Vulkan 验证层（发布版建议关）
  RHI_EXIT_AFTER=<帧数>       跑 N 帧后自动退出（冒烟/回归用）
"@ | Set-Content -Encoding UTF8 (Join-Path $dist "README.txt")

$zip = Join-Path $root "dist/$AppName-win.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $dist "*") -DestinationPath $zip

Write-Host "打包完成："
Write-Host "  目录 $dist"
Write-Host "  压缩包 $zip"
