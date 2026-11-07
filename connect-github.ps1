param(
    [string]$RepoName = "browser-cpp",
    [ValidateSet("private", "public")]
    [string]$Visibility = "private"
)

$ErrorActionPreference = "Stop"

function Require-Command([string]$Name, [string]$InstallHint) {
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        Write-Host "$Name is not installed." -ForegroundColor Red
        Write-Host $InstallHint -ForegroundColor Yellow
        exit 1
    }
}

Require-Command "git" "Install Git for Windows: https://git-scm.com/download/win"
Require-Command "gh"  "Install GitHub CLI: winget install --id GitHub.cli"

Write-Host ""
Write-Host "BROWSER / GITHUB SETUP" -ForegroundColor Cyan
Write-Host "----------------------" -ForegroundColor DarkGray

& gh auth status --hostname github.com *> $null
if ($LASTEXITCODE -ne 0) {
    Write-Host "Sign in to GitHub in the browser window..." -ForegroundColor Yellow
    & gh auth login --hostname github.com --git-protocol https --web
    if ($LASTEXITCODE -ne 0) { throw "GitHub sign-in failed." }
}

$owner = (& gh api user --jq .login).Trim()
if (-not $owner) { throw "Could not read the authenticated GitHub account." }
$fullName = "$owner/$RepoName"

# Refuse to overwrite an existing remote repository. This avoids accidental pushes.
& gh repo view $fullName --json name *> $null
if ($LASTEXITCODE -eq 0) {
    throw "GitHub repository $fullName already exists. Choose another -RepoName or connect it manually."
}

if (-not (Test-Path ".git")) {
    & git init --initial-branch=main
    if ($LASTEXITCODE -ne 0) { throw "git init failed." }
}

& git branch -M main
if ($LASTEXITCODE -ne 0) { throw "Could not set the main branch." }

$authorName = (& git config user.name 2>$null)
if (-not $authorName) {
    $authorName = (& gh api user --jq '.name // .login').Trim()
    & git config user.name $authorName
}

$authorEmail = (& git config user.email 2>$null)
if (-not $authorEmail) {
    $authorEmail = (& gh api user --jq '.email // empty').Trim()
    if (-not $authorEmail) {
        Write-Host "Git needs an author email for commits." -ForegroundColor Yellow
        $authorEmail = Read-Host "Email (or GitHub noreply email)"
    }
    if (-not $authorEmail) { throw "A Git author email is required." }
    & git config user.email $authorEmail
}

& git add -A
if ($LASTEXITCODE -ne 0) { throw "git add failed." }

& git rev-parse --verify HEAD *> $null
if ($LASTEXITCODE -ne 0) {
    & git commit -m "Initial commit: Browser 1.0"
    if ($LASTEXITCODE -ne 0) { throw "Initial commit failed." }
} else {
    & git diff --cached --quiet
    if ($LASTEXITCODE -ne 0) {
        & git commit -m "Update Browser 1.0"
        if ($LASTEXITCODE -ne 0) { throw "Git commit failed." }
    }
}

$visibilityFlag = if ($Visibility -eq "public") { "--public" } else { "--private" }

Write-Host "Creating $Visibility repository: $fullName" -ForegroundColor Cyan
& gh repo create $fullName $visibilityFlag --source . --remote origin --push `
    --description "Fast C++20 console web browser for Windows"
if ($LASTEXITCODE -ne 0) {
    throw "GitHub repository creation or push failed. Check GitHub CLI permissions and try again."
}

Write-Host ""
Write-Host "Repository created and pushed:" -ForegroundColor Green
Write-Host "https://github.com/$fullName" -ForegroundColor Cyan
Write-Host ""
Write-Host "Future updates:" -ForegroundColor Yellow
Write-Host "  git add ."
Write-Host '  git commit -m "Describe your change"'
Write-Host "  git push"
