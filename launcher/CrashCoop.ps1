# Crash Coop - petite fenêtre pour lancer le mod de co-op (Crash Bandicoot N. Sane Trilogy, Steam). FR / EN.
# Double-clic sur "Lancer Crash Coop.cmd". Le chargement réel du mod est fait par coop.ps1 (même dossier).
# Rien n'est écrit dans le dossier du jeu ni dans les sauvegardes.
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
[Windows.Forms.Application]::EnableVisualStyles()

$here     = Split-Path -Parent $MyInvocation.MyCommand.Path
$coop     = Join-Path $here 'coop.ps1'
$logFile  = Join-Path $here 'coop-me.log'
$stopFile = Join-Path $here 'coop.stop'
$setFile  = Join-Path $here 'crashcoop-ami.txt'
$langFile = Join-Path $here 'crashcoop-lang.txt'
$gameName = 'CrashBandicootNSaneTrilogy'

# ---------- textes ----------
$S = @{
  fr = @{
    peerLbl = "SteamID64 de ton ami (ou le lien de son profil Steam) :"
    game = '1. Lancer le jeu'; mod = '2. Activer le mod'; stop = 'Arrêter le mod'
    auto = 'Activer le mod tout seul quand le jeu est lancé'; state = 'État'
    coco = 'Voir mon ami en Coco (pris en compte à la prochaine activation du mod)'
    copy = 'Copier mon SteamID'; logs = 'Ouvrir le dossier (journaux)'; langBtn = 'English'
    gameOn = '● Jeu : lancé'; gameOff = '○ Jeu : pas lancé'
    modOn = '● Mod : actif'; modStarting = '◐ Mod : démarrage...'; modErr = '✖ Mod : erreur ({0})'; modOff = '○ Mod : inactif'
    frWait = '○ Ami : pas encore connecté (il doit aussi activer le mod)'; frVis = '● Ami : connecté, visible dans ton niveau'
    frOther = '◐ Ami : connecté, mais pas dans le même niveau'; frNone = '○ Ami : -'
    me = 'Ton SteamID : {0}'; meNone = 'Ton SteamID : (affiché une fois le mod actif)'
    fault = 'Une erreur a été notée dans coop-me.log : envoie-le.'
    needPeer = "Entre d'abord le SteamID64 de ton ami (17 chiffres qui commencent par 7656119)."
    needGame = "Lance d'abord le jeu (bouton 1)."; already = 'Le mod est déjà actif.'
    loading = 'Chargement du mod...'; loaded = 'Mod chargé.'; failed = 'Échec du chargement (détails dans la fenêtre).'
    failedBox = "Le mod n'a pas pu être chargé :"; running = 'Le jeu tourne déjà.'
    starting = 'Démarrage du jeu via Steam... (le logo sur fond blanc se passe avec Entrée)'
    stopAsked = "Arrêt demandé (le Crash de ton ami disparaît d'ici une seconde)."; copied = 'SteamID copié : envoie-le à ton ami.'
  }
  en = @{
    peerLbl = "Your friend's SteamID64 (or their Steam profile link):"
    game = '1. Start the game'; mod = '2. Enable the mod'; stop = 'Stop the mod'
    auto = 'Enable the mod automatically when the game starts'; state = 'Status'
    coco = 'Show my friend as Coco (applies the next time the mod is enabled)'
    copy = 'Copy my SteamID'; logs = 'Open folder (logs)'; langBtn = 'Français'
    gameOn = '● Game: running'; gameOff = '○ Game: not running'
    modOn = '● Mod: active'; modStarting = '◐ Mod: starting...'; modErr = '✖ Mod: error ({0})'; modOff = '○ Mod: inactive'
    frWait = '○ Friend: not connected yet (they must enable the mod too)'; frVis = '● Friend: connected, visible in your level'
    frOther = '◐ Friend: connected, but in another level'; frNone = '○ Friend: -'
    me = 'Your SteamID: {0}'; meNone = 'Your SteamID: (shown once the mod is active)'
    fault = 'An error was logged in coop-me.log: please send it.'
    needPeer = "First enter your friend's SteamID64 (17 digits starting with 7656119)."
    needGame = 'Start the game first (button 1).'; already = 'The mod is already active.'
    loading = 'Loading the mod...'; loaded = 'Mod loaded.'; failed = 'Loading failed (details in the popup).'
    failedBox = 'The mod could not be loaded:'; running = 'The game is already running.'
    starting = 'Starting the game through Steam... (press Enter to skip the white logo screen)'
    stopAsked = "Stop requested (your friend's Crash disappears within a second)."; copied = 'SteamID copied: send it to your friend.'
  }
}
$script:lang = if (Test-Path $langFile) { (Get-Content $langFile -TotalCount 1).Trim() } elseif ((Get-Culture).TwoLetterISOLanguageName -eq 'fr') { 'fr' } else { 'en' }
if (-not $S.ContainsKey($script:lang)) { $script:lang = 'fr' }
function T([string]$k) { $S[$script:lang][$k] }

# ---------- état ----------
$script:autoDonePid = 0        # pid du jeu déjà traité par le mode auto (une seule tentative par lancement du jeu)
$script:userStopped = $false   # l'utilisateur a cliqué "Arrêter" : le mode auto ne relance pas
$script:lastRx = -1; $script:rxGrowing = $false; $script:mySteamId = ''
$script:infoKey = $null; $script:infoColor = $null   # dernier message d'info (re-traduit si on change de langue)

function Get-Game { Get-Process $gameName -ErrorAction SilentlyContinue | Select-Object -First 1 }

function Parse-Peer([string]$s) {
    # accepte le SteamID64 seul ou un lien de profil steamcommunity.com/profiles/7656119...
    if ($s -match '(7656119\d{10})') { return $Matches[1] } else { return $null }
}

function Read-Log {
    if (-not (Test-Path $logFile)) { return @() }
    try {
        $fs = [IO.File]::Open($logFile, 'Open', 'Read', 'ReadWrite')
        $sr = New-Object IO.StreamReader($fs); $t = $sr.ReadToEnd(); $sr.Close()
        return ($t -split "`r?`n" | Where-Object { $_ })
    } catch { return @() }
}

# Le mod est-il actif dans le jeu en cours ? (journal écrit depuis le démarrage du jeu, pas terminé par "stopped")
function Get-ModState($game, $lines) {
    if (-not $game -or -not (Test-Path $logFile)) { return 'off' }
    $lw = (Get-Item $logFile).LastWriteTime
    try { if ($lw -lt $game.StartTime) { return 'off' } } catch {}
    if ($lines.Count -eq 0) { return 'starting' }
    $last = $lines[-1]
    if ($last -eq 'stopped') { return 'off' }
    if ($lines | Where-Object { $_ -match '^(steam init failed|bad peer|missing steam export|steam_api64.dll not loaded)' }) { return 'error' }
    return 'on'
}

function Run-Coop([string[]]$argv) {
    $psi = New-Object Diagnostics.ProcessStartInfo 'powershell.exe'
    $psi.Arguments = '-NoProfile -ExecutionPolicy Bypass -File "' + $coop + '" ' + ($argv -join ' ')
    $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
    $p = [Diagnostics.Process]::Start($psi)
    $out = $p.StandardOutput.ReadToEnd(); $err = $p.StandardError.ReadToEnd(); $p.WaitForExit()
    return @{ ok = ($p.ExitCode -eq 0 -and -not $err); text = ($out + $err).Trim() }
}

# ---------- fenêtre ----------
$f = New-Object Windows.Forms.Form
$f.Text = 'Crash Coop'; $f.ClientSize = New-Object Drawing.Size(460, 426)
$f.FormBorderStyle = 'FixedSingle'; $f.MaximizeBox = $false; $f.StartPosition = 'CenterScreen'
$f.Font = New-Object Drawing.Font('Segoe UI', 9.5)
$f.BackColor = [Drawing.Color]::FromArgb(250, 246, 240)

function Add-Ctl($ctl, $x, $y, $w, $h) { $ctl.Location = New-Object Drawing.Point($x, $y); $ctl.Size = New-Object Drawing.Size($w, $h); $f.Controls.Add($ctl); $ctl }

$title = Add-Ctl (New-Object Windows.Forms.Label) 16 12 330 28
$title.Text = 'Crash Bandicoot - co-op'; $title.Font = New-Object Drawing.Font('Segoe UI Semibold', 14)
$title.ForeColor = [Drawing.Color]::FromArgb(200, 80, 20)
$btnLang = Add-Ctl (New-Object Windows.Forms.Button) 354 12 90 28

$lblPeer = Add-Ctl (New-Object Windows.Forms.Label) 16 50 430 20
$txtPeer = Add-Ctl (New-Object Windows.Forms.TextBox) 16 72 428 26
if (Test-Path $setFile) { $txtPeer.Text = (Get-Content $setFile -TotalCount 1) }

$btnGame = Add-Ctl (New-Object Windows.Forms.Button) 16 110 136 34
$btnMod  = Add-Ctl (New-Object Windows.Forms.Button) 162 110 136 34
$btnStop = Add-Ctl (New-Object Windows.Forms.Button) 308 110 136 34

$chkAuto = Add-Ctl (New-Object Windows.Forms.CheckBox) 16 152 428 22; $chkAuto.Checked = $true
# coop.coco next to the dll = the mod shows the friend as Coco (read when the mod starts)
$cocoFile = Join-Path $here 'coop.coco'
$chkCoco = Add-Ctl (New-Object Windows.Forms.CheckBox) 16 176 428 22; $chkCoco.Checked = (Test-Path $cocoFile)
$chkCoco.Add_CheckedChanged({ if ($chkCoco.Checked) { New-Item $cocoFile -ItemType File -Force | Out-Null } else { Remove-Item $cocoFile -ErrorAction SilentlyContinue } })

$grp = Add-Ctl (New-Object Windows.Forms.GroupBox) 16 206 428 164
function Add-Row($y) { $l = New-Object Windows.Forms.Label; $l.Location = New-Object Drawing.Point(12, $y); $l.Size = New-Object Drawing.Size(404, 22); $grp.Controls.Add($l); $l }
$rowGame = Add-Row 24; $rowMod = Add-Row 50; $rowFriend = Add-Row 76; $rowMe = Add-Row 102; $rowInfo = Add-Row 128

$btnCopy = Add-Ctl (New-Object Windows.Forms.Button) 16 380 210 32; $btnCopy.Enabled = $false
$btnLog  = Add-Ctl (New-Object Windows.Forms.Button) 234 380 210 32

$green = [Drawing.Color]::FromArgb(30, 130, 60); $red = [Drawing.Color]::FromArgb(190, 40, 40)
$gray  = [Drawing.Color]::FromArgb(100, 100, 100); $amber = [Drawing.Color]::FromArgb(180, 110, 0)
function Set-Row($row, $text, $color) { $row.Text = $text; $row.ForeColor = $color }
function Set-Info($key, $color) { $script:infoKey = $key; $script:infoColor = $color; Set-Row $rowInfo (T $key) $color }

function Apply-Lang {
    $btnLang.Text = T 'langBtn'; $lblPeer.Text = T 'peerLbl'
    $btnGame.Text = T 'game'; $btnMod.Text = T 'mod'; $btnStop.Text = T 'stop'
    $chkAuto.Text = T 'auto'; $chkCoco.Text = T 'coco'; $grp.Text = T 'state'; $btnCopy.Text = T 'copy'; $btnLog.Text = T 'logs'
    if ($script:infoKey) { Set-Row $rowInfo (T $script:infoKey) $script:infoColor }
}

function Save-Peer { $id = Parse-Peer $txtPeer.Text; if ($id) { Set-Content $setFile $id -Encoding ascii; $txtPeer.Text = $id }; $id }

function Start-Mod([switch]$Quiet) {
    $id = Save-Peer
    if (-not $id) { if (-not $Quiet) { [Windows.Forms.MessageBox]::Show((T 'needPeer'), 'Crash Coop') | Out-Null }; return }
    $g = Get-Game
    if (-not $g) { if (-not $Quiet) { [Windows.Forms.MessageBox]::Show((T 'needGame'), 'Crash Coop') | Out-Null }; return }
    if ((Get-ModState $g (Read-Log)) -eq 'on') { if (-not $Quiet) { [Windows.Forms.MessageBox]::Show((T 'already'), 'Crash Coop') | Out-Null }; return }
    # si on vient de l'arrêter, attendre qu'il ait vraiment fini avant d'effacer coop.stop
    if (Test-Path $stopFile) { for ($i = 0; $i -lt 30 -and (Get-ModState $g (Read-Log)) -ne 'off'; $i++) { Start-Sleep -Milliseconds 200 } }
    $f.Cursor = 'WaitCursor'; Set-Info 'loading' $gray; $f.Refresh()
    $r = Run-Coop @('-Peer', $id)
    $f.Cursor = 'Default'; $script:userStopped = $false; $script:lastRx = -1
    if ($r.ok) { Set-Info 'loaded' $green }
    else { Set-Info 'failed' $red; [Windows.Forms.MessageBox]::Show((T 'failedBox') + "`n`n" + $r.text, 'Crash Coop') | Out-Null }
}

$btnLang.Add_Click({
    $script:lang = if ($script:lang -eq 'fr') { 'en' } else { 'fr' }
    Set-Content $langFile $script:lang -Encoding ascii
    Apply-Lang; Refresh-State
})
$btnGame.Add_Click({
    if (Get-Game) { Set-Info 'running' $gray; return }
    Save-Peer | Out-Null
    Start-Process 'steam://rungameid/731490'
    Set-Info 'starting' $gray
})
$btnMod.Add_Click({ Start-Mod })
$btnStop.Add_Click({
    $script:userStopped = $true
    New-Item -ItemType File $stopFile -Force | Out-Null
    Set-Info 'stopAsked' $gray
})
$btnCopy.Add_Click({ if ($script:mySteamId) { [Windows.Forms.Clipboard]::SetText($script:mySteamId); Set-Info 'copied' $green } })
$btnLog.Add_Click({ Start-Process explorer.exe $here })
$txtPeer.Add_Leave({ Save-Peer | Out-Null })

# ---------- rafraîchissement toutes les secondes ----------
function Refresh-State {
    $g = Get-Game; $lines = Read-Log; $state = Get-ModState $g $lines

    if ($g) { Set-Row $rowGame (T 'gameOn') $green } else { Set-Row $rowGame (T 'gameOff') $gray }

    switch ($state) {
        'on'       { Set-Row $rowMod (T 'modOn') $green }
        'starting' { Set-Row $rowMod (T 'modStarting') $amber }
        'error'    { $e = ($lines | Where-Object { $_ -match '^(steam init failed|bad peer|missing steam export|steam_api64)' } | Select-Object -First 1)
                     Set-Row $rowMod ((T 'modErr') -f $e) $red }
        default    { Set-Row $rowMod (T 'modOff') $gray }
    }

    $me = $lines | ForEach-Object { if ($_ -match 'my SteamID (\d+)') { $Matches[1] } } | Select-Object -Last 1
    if ($me) { $script:mySteamId = $me; Set-Row $rowMe ((T 'me') -f $me) $gray; $btnCopy.Enabled = $true }
    else { Set-Row $rowMe (T 'meNone') $gray }

    if ($state -eq 'on') {
        $t = $lines | Where-Object { $_ -match '^t=\d+s ' } | Select-Object -Last 1
        $rx = if ($t -match 'received (\d+)') { [int64]$Matches[1] } else { 0 }
        if ($script:lastRx -ge 0) { $script:rxGrowing = $rx -gt $script:lastRx }
        $script:lastRx = $rx
        $vis = $lines | Where-Object { $_ -match '^friend (visible|hidden)' } | Select-Object -Last 1
        $fault = $lines | Where-Object { $_ -match '^FAULT' } | Select-Object -Last 1
        if (-not $script:rxGrowing) { Set-Row $rowFriend (T 'frWait') $amber }
        elseif ($vis -match 'visible') { Set-Row $rowFriend (T 'frVis') $green }
        else { Set-Row $rowFriend (T 'frOther') $amber }
        if ($fault) { Set-Info 'fault' $red }
    } else { Set-Row $rowFriend (T 'frNone') $gray; $script:lastRx = -1 }

    # mode auto : une tentative par lancement du jeu, 20 s après son démarrage
    if ($chkAuto.Checked -and $g -and $state -eq 'off' -and -not $script:userStopped -and $script:autoDonePid -ne $g.Id) {
        $age = 0; try { $age = ((Get-Date) - $g.StartTime).TotalSeconds } catch { $age = 999 }
        if ($age -ge 20 -and (Parse-Peer $txtPeer.Text)) { $script:autoDonePid = $g.Id; Start-Mod -Quiet }
    }
    if (-not $g) { $script:userStopped = $false }
}

Apply-Lang
$timer = New-Object Windows.Forms.Timer; $timer.Interval = 1000
$timer.Add_Tick({ Refresh-State }); $timer.Start()
$f.Add_Shown({ Refresh-State; if (-not $txtPeer.Text) { $txtPeer.Focus() } })
[void]$f.ShowDialog()
$timer.Stop()
