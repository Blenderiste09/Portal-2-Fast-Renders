# P2Render : rendu ultra-rapide de démos Portal 2

P2Render est une application indépendante qui transforme des démos Portal 2 en vidéos MP4 (H.264 ou
HEVC, avec le son) beaucoup plus vite que `startmovie`. Les images ne passent jamais par le
processeur : le jeu tourne en Direct3D 9Ex, chaque image est copiée sur la carte graphique dans une
texture partagée, puis encodée par l'encodeur matériel de la carte (NVENC / AMF / QuickSync).

Rien n'est installé dans le dossier du jeu et il n'y a aucune commande à taper dans la console.

## Utilisation

1. Lancer `P2Render.exe`. Steam doit être ouvert, et Portal 2 doit être fermé.
2. Ajouter des démos : bouton **Ajouter des démos**, ou glisser des fichiers `.dem` dans la liste.
3. Choisir la résolution, les images par seconde, le format, la qualité et le dossier des vidéos.
4. Cliquer sur **Lancer le rendu**.

P2Render démarre Portal 2, rend les démos l'une après l'autre (progression, vitesse et temps
restant affichés), puis ferme le jeu si l'option est cochée. Une fois une démo terminée, ses
boutons permettent de lire la vidéo ou de l'afficher dans son dossier.

## Vitesse

Mesures sur un Ryzen 7 5700X avec une carte NVIDIA, sur une démo de 12,2 s à 60 images/s :

| Méthode | Vitesse |
|---|---|
| `startmovie` du jeu (images TGA, à encoder ensuite) | ~126 images/s |
| P2Render en 720p | ~570 images/s (9x le temps réel) |
| P2Render en 1080p | ~540 images/s |

En comptant le lancement de Portal 2, 2 démos sont rendues en 12 s environ.

## Compiler

Il faut Visual Studio 2026 avec les outils C++ (x86 et x64).

```powershell
.\build.ps1          # produit dist\ (P2Render.exe et ses fichiers)
.\build.ps1 -Run     # compile puis lance P2Render
```

Le dossier `dist\` se copie tel quel où l'on veut.

## Journaux en cas de problème

Ils sont dans le dossier de P2Render :
- `p2render.log` (application) ;
- `p2r_core.log` (côté jeu) ;
- `p2r_encoder.log` (encodeur).

Les démos enregistrées avant la mise à jour 2026 du jeu ne se lisent plus (erreur `DataTable` du jeu) :
P2Render les signale comme incompatibles.
