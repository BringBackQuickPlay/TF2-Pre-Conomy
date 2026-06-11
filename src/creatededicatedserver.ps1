# Use this manually if PowerShell refuses to execute the script:
# Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass

$ErrorActionPreference = "Stop"

Write-Host "Generating Windows dedicated-server projects..."

& ".\devtools\bin\vpc.exe" `
    /tf `
    /define:SOURCESDK `
    +dedicated `
    /dedicated `
    /mksln dedicated.sln

if ($LASTEXITCODE -ne 0)
{
    throw "VPC failed with exit code $LASTEXITCODE."
}

# The dedicated VPC group contains:
# - mathlib
# - server
# - tier1
#
# Verify all three dedicated project files were actually generated before
# allowing MSBuild to start.
$requiredProjects = @(
    ".\mathlib\mathlib_win64_srv.vcxproj",
    ".\tier1\tier1_win64_srv.vcxproj",
    ".\game\server\server_win64_srv_tf.vcxproj"
)

$missingProjects = @(
    $requiredProjects | Where-Object { -not (Test-Path -LiteralPath $_) }
)

if ($missingProjects.Count -gt 0)
{
    Write-Host ""
    Write-Host "Generated project files:" -ForegroundColor Yellow

    Get-ChildItem `
        -Path ".\mathlib", ".\tier1", ".\game\server" `
        -Filter "*.vcxproj" `
        -ErrorAction SilentlyContinue |
        ForEach-Object {
            Write-Host "  $($_.FullName)"
        }

    Write-Host ""
    throw (
        "VPC did not generate the following required dedicated-server projects:`n  " +
        ($missingProjects -join "`n  ")
    )
}

# This must modify the dedicated server project, not the normal server project.
$vcxprojPath = Resolve-Path `
    ".\game\server\server_win64_srv_tf.vcxproj"

Write-Host "Adding DEDICATED to:"
Write-Host "  $vcxprojPath"

[xml]$vcxproj = Get-Content -LiteralPath $vcxprojPath

$updated = $false

foreach ($group in $vcxproj.Project.ItemDefinitionGroup)
{
    if ($null -eq $group.ClCompile)
    {
        continue
    }

    $preprocessor = [string]$group.ClCompile.PreprocessorDefinitions

    if ($preprocessor -notmatch "(^|;)DEDICATED(;|$)")
    {
        if ([string]::IsNullOrWhiteSpace($preprocessor))
        {
            $group.ClCompile.PreprocessorDefinitions = "DEDICATED"
        }
        else
        {
            $group.ClCompile.PreprocessorDefinitions =
                "DEDICATED;$preprocessor"
        }

        $updated = $true
    }
}

if ($updated)
{
    $vcxproj.Save($vcxprojPath)

    Write-Host "Added DEDICATED to the dedicated server project."
}
else
{
    Write-Host "DEDICATED was already present."
}

Write-Host ""
Write-Host "Dedicated-server project generation completed successfully."
Write-Host "Generated solution:"
Write-Host "  $(Resolve-Path '.\dedicated.sln')"
