# Autentikasi Git/GitHub yang Aman untuk Mectov OS

> **Kenapa dokumen ini ada:** token klasik yang diketik langsung di chat (atau di URL remote)
> bisa bocor ke history, log, atau screenshot. Dokumen ini menjelaskan dua cara yang **tidak**
> pernah mengekspos kredensial di chat atau di disk plaintext: **SSH key** (paling direkomendasikan)
> dan **Git Credential Manager / credential helper** (paling mudah di Windows).

---

## Ringkasan cepat

| Metode | Keamanan | Platform | Cocok untuk |
|---|---|---|---|
| **SSH key** ⭐ | Tertinggi — key private tidak pernah keluar dari mesin | Linux / macOS / WSL | Developer serius |
| **Git Credential Manager** | Tinggi — token disimpan terenkripsi oleh OS | Windows / macOS / Linux | Pemula, pemakaian jarang |
| Token di URL remote | ❌ Rendah — token plaintext di `.git/config` | semua | **Hindari** |

Setelah setup selesai, perintah push kamu tetap sama:

```bash
git push origin main
```

---

## Metode 1 (disarankan): SSH key

SSH tidak pernah memakai password/token — otentikasi pakai pasangan kunci
`private`/`public`. Kunci private tidak pernah meninggalkan mesin kamu.

### 1. Cek apakah kamu sudah punya kunci

```bash
ls -la ~/.ssh/id_ed25519.pub   # atau id_rsa.pub
```

### 2. Generate kunci (kalau belum ada)

```bash
ssh-keygen -t ed25519 -C "MAliffadlan@mectov" -f ~/.ssh/id_ed25519
# Tekan Enter untuk lokasi default, lalu buat passphrase (disarankan!)
```

**Jangan pernah kirim isi `id_ed25519` (private key) ke siapa pun atau ke chat.**

### 3. Tambahkan public key ke GitHub

1. Buka https://github.com/settings/keys → **New SSH key**
2. Isi Title (mis. "Laptop utama") dan tempel isi file ini:

```bash
cat ~/.ssh/id_ed25519.pub
```

### 4. Aktifkan SSH agent (agar passphrase tidak ditanya terus)

```bash
eval "$(ssh-agent -s)"
ssh-add ~/.ssh/id_ed25519
```

Agar otomatis tiap login, tambahkan baris itu ke `~/.bashrc` (atau `~/.zshrc`).

### 5. Ganti remote ke SSH dan tes

```bash
git remote set-url origin git@github.com:MAliffadlan/Mectov_OS.git
ssh -T git@github.com
# Harusnya mencetak: "Hi MAliffadlan! You've successfully authenticated..."
git push origin main
```

### 6. (Opsional) Pasang kunci di banyak mesin / CI

- Mesin lain: ulangi langkah 3 dengan public key dari mesin itu.
- GitHub Actions: gunakan **deploy key** per repo (Settings → Deploy keys) dengan
  kunci terpisah, atau `actions/checkout` dengan `ssh-key` dari GitHub Secrets —
  **jangan** hardcode kunci di workflow.

---

## Metode 2: Git Credential Manager (token disimpan aman oleh OS)

Git Credential Manager (GCM) menyimpan token di credential store terenkripsi
sistem operasi (Windows Credential Manager / macOS Keychain / libsecret), jadi
token **tidak pernah** ditulis ke `.git/config` dan tidak perlu diketik ulang.

### Windows / macOS

```bash
# Windows (Git for Windows biasanya sudah termasuk GCM; cek dulu):
git credential-manager version

# macOS:
brew install --cask git-credential-manager
```

Kalau belum terpasang di Windows, install dari
https://github.com/git-credential-manager/git-credential-manager/releases
(pilih `GCM-Windows-*.exe`).

### Linux (Debian/Ubuntu)

```bash
sudo apt-get install git-credential-manager  # atau ikuti release .deb di atas
```

### Aktifkan dan pakai

```bash
git config --global credential.helper manager   # Windows: "manager-core"
git remote set-url origin https://github.com/MAliffadlan/Mectov_OS.git
git push origin main
```

Saat diminta, masukkan **username** kamu dan **Personal Access Token (PAT)**
sebagai password (bukan password akun!). GCM menyimpan token itu — pertama kali
saja, selanjutnya otomatis.

### Membuat PAT yang benar (bukan token klasik sembarangan)

1. https://github.com/settings/tokens → **Generate new token** → **Fine-grained**
2. Repository access: **Only select repositories** → pilih `Mectov_OS`
3. Permissions → **Contents: Read and write** (cukup untuk push; jangan beri
   admin/delete kecuali perlu)
4. Set **Expiration** sesingkat mungkin (mis. 30–90 hari)
5. Simpan token di password manager — jangan di chat, jangan di file repo!

> Token klasik (`ghp_...`) tetap bekerja tapi tidak bisa dibatasi per-repo/permission
> sedetail fine-grained. Untuk proyek ini, fine-grained lebih aman.

---

## Metode 3 (⚠️ hindari): token di URL remote

```bash
# JANGAN lakukan ini — token tersimpan plaintext di .git/config:
git remote set-url origin https://MAliffadlan:TOKEN@github.com/MAliffadlan/Mectov_OS.git
```

Risiko:
- Token tercetak di `.git/config` (disk, backup, screenshot).
- Bisa ikut ter-expose lewat `git remote -v`.
- Kalau remote ini pernah di-share/duplikat, token ikut bocor.

**Kalau terlanjur melakukan ini** (mis. lewat push inline sekali pakai), token
tetap tidak tersimpan kalau URL dipakai langsung di perintah `git push` (bukan
`set-url`). Tapi jika sudah terlanjur `set-url`, bersihkan:

```bash
git remote set-url origin https://github.com/MAliffadlan/Mectov_OS.git
git remote -v   # pastikan tidak ada token
```

---

## Jika token pernah bocor (mis. terkirim di chat)

1. **Revoke segera:** https://github.com/settings/tokens → hapus token tersebut.
2. Cek audit log GitHub (Settings → Security log) untuk pemakaian mencurigakan.
3. Cek apakah `.git/config` atau history lokal masih menyimpannya:

```bash
git remote -v
grep -rn "ghp_" .git/config 2>/dev/null
```

4. Kalau ada di history git (mis. commit yang isinya URL+token), **rotate**
   token dan tulis ulang history lokal sebelum push berikutnya
   (`git filter-repo` — hati-hati, hanya untuk commit yang belum di-push ke
   tempat lain; untuk yang sudah ter-push, revoke token adalah satu-satunya obat).

---

## Troubleshooting

| Gejala | Solusi |
|---|---|
| `Permission denied (publickey)` | `ssh -T git@github.com`; pastikan public key ada di GitHub dan private key di `~/.ssh` |
| `could not read Username for 'https://github.com'` | Belum ada credential helper; pakai Metode 1 atau 2 |
| Passphrase diminta terus | `ssh-add` (Metode 1 langkah 4) |
| GCM minta password terus-menerus | Pastikan pakai PAT sebagai password, bukan password akun |
| Push ditolak `403` | Token tidak punya permission Contents:write; buat ulang PAT fine-grained |

---

## Rekap untuk repo ini

```bash
# Pilihan terbaik: SSH
git remote set-url origin git@github.com:MAliffadlan/Mectov_OS.git
ssh -T git@github.com

# Lalu push seperti biasa
git push origin main
```
