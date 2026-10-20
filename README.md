# Portal 2 D3D9Ex : rendu rapide de démos

Rend une démo Portal 2 en MP4 (H.264 ou HEVC + son AAC) sans jamais passer les images par le CPU :
le jeu tourne en Direct3D 9Ex, chaque image est copiée sur le GPU dans une texture partagée, et un
encodeur x64 séparé l'encode avec l'encodeur matériel de la carte (NVENC / AMF / QuickSync via Media
Foundation).

Mesures (Ryzen 7 5700X, carte NVIDIA, démo de 12,2 s à 60 fps) :

| Méthode | Vitesse |
|---|---|
| `startmovie x raw` du jeu (TGA + WAV, à encoder ensuite) | ~126 images/s |
| `p2r_start`, 720p, fenêtre mise à jour | 377 images/s |
| `p2r_start`, 720p, `window=0` | 550 images/s (9x le temps réel) |
| `p2r_start`, 1080p, `window=0` | 537 images/s |

## Installation

1. `.\build.ps1 -Deploy` (Visual Studio 2026 requis) copie dans `<Portal 2>\bin` :
   `d3d9.dll` (le proxy qui passe le jeu en D3D9Ex) et `p2r_encoder.exe`.
2. Lancer Portal 2 normalement. Pour revenir au D3D9 d'origine : option de lancement `-nod3d9ex`
   ou `.\build.ps1 -Undeploy`.

## Utilisation

Dans la console :

```
p2r_start <nom> demo=<démo> [fps=60] [codec=h264|hevc] [bitrate=<kbit/s>] [window=1|0]
```

- lance la démo, démarre l'enregistrement quand elle est en jeu, s'arrête tout seul à la fin ;
- sans `demo=`, enregistre ce qui se passe jusqu'à `p2r_stop` ;
- `window=0` ne rafraîchit plus la fenêtre pendant le rendu (plus rapide) ;
- le débit par défaut dépend de la résolution (1080p60 : ~41 Mbit/s).

La vidéo est écrite dans `<Portal 2>\p2r_movies\<nom>.mp4`. La console affiche la vitesse du
rendu puis `video written`.

Pour automatiser : `-p2r_exec <cfg>` exécute un cfg une fois les commandes disponibles, et
`p2r_after <images> <commande>` exécute une commande plus tard (Portal 2 n'a pas de `wait`).

Journaux : `<Portal 2>\d3d9ex.log` (jeu) et `<Portal 2>\bin\p2r_encoder.log` (encodeur).
