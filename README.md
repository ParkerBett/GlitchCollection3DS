# Glitch Collection

A Nintendo 3DS application for watching shows produced by **Glitch Productions**.

Videos and artwork are bundled directly into the application using RomFS, so no separate media folder is required on the SD card.

## Features

- Browse shows and episodes.
- Display show and episode artwork.
- Play videos with audio.
- Pause and resume playback.
- Seek forwards and backwards.
- Tap the progress bar to seek.
- Display playback progress and duration.
- Touchscreen and button controls.

## Controls

### Menus

| Control | Action |
|---|---|
| D-pad / Circle Pad | Navigate |
| A / Touch | Select |
| B | Return to show list |
| START | Exit |

### Video player

| Control | Action |
|---|---|
| A / Touch | Pause or resume |
| D-pad Left | Seek backwards 10 seconds |
| D-pad Right | Seek forwards 10 seconds |
| Touch progress bar | Seek to position |
| B | Return to episode list |
| START | Exit |

Seeking uses video keyframes, so playback may resume slightly before the selected position.

Episodes do not automatically advance when finished.

## Media

Media is stored in the following structure:

```text
romfs/
└── GlitchCollection/
    └── show_name/
        ├── main.jpg
        ├── 1.jpg
        ├── 1.ogv
        └── ...
```

- `main.jpg`: Show artwork.
- Numbered `.jpg` files: Episode artwork.
- Numbered `.ogv` files: Episodes.

### Media formats

- Artwork: 512 × 256 JPEG.
- Video: 400 × 224, 24 FPS.
- Video codec: Theora.
- Audio codec: Vorbis.
- Container: Ogg.

Each episode contains both video and audio in one `.ogv` file.

## Requirements

- devkitPro and devkitARM.
- libctru, citro2d and citro3d.
- FFmpeg libraries built for the 3DS.
- libjpeg-turbo, libogg, libvorbisidec and libtheoradec.
- `makerom`, `bannertool` and `3dstool` for CIA packaging.

Install the additional devkitPro packages:

```bash
pacman -S 3ds-libjpeg-turbo 3ds-libvorbisidec 3ds-libogg 3ds-libtheora
```

## Building

Open the devkitPro MSYS2 terminal in the project directory and run:

```bash
make clean
make
```

This builds the Homebrew Launcher `.3dsx` application.

### Creating a CIA

After building, run the following commands from the project directory in Windows Command Prompt.

Generate the application icon and banner:

```cmd
bannertool makesmdh -s "Glitch Collection" -l "Glitch Collection" -p "ParkerBett" -i icon.png -o GlitchCollection.smdh

bannertool makebanner -i banner.png -a banner.wav -o banner.bnr
```

Create the RomFS image:

```cmd
3dstool -c -t romfs -f romfs.bin --romfs-dir romfs
```

Create the base CXI:

```cmd
makerom -f cxi -o GlitchCollection-base.cxi -rsf app.rsf -target t -exefslogo -elf GlitchCollection.elf -icon GlitchCollection.smdh -banner banner.bnr
```

Extract and decompress the executable:

```cmd
mkdir cxi_extract

3dstool -x -t cxi -f GlitchCollection-base.cxi --header cxi_extract\header.bin --exh cxi_extract\exheader.bin --exefs cxi_extract\exefs.bin

3dstool -x -t exefs -f cxi_extract\exefs.bin --exefs-dir cxi_extract\exefs

3dstool -u -f cxi_extract\exefs\code.bin --compress-type blz --compress-out cxi_extract\code_decompressed.bin
```

Rebuild the CXI with RomFS and create the CIA:

```cmd
makerom -f cxi -o GlitchCollection.cxi -rsf app.rsf -target t -code cxi_extract\code_decompressed.bin -exheader cxi_extract\exheader.bin -icon GlitchCollection.smdh -banner banner.bnr -romfs romfs.bin

makerom -f cia -o GlitchCollection.cia -target t -content GlitchCollection.cxi:0:0
```

The resulting `GlitchCollection.cia` can be installed using FBI.

## Technical Details

- Written in C.
- Uses libctru, citro2d and citro3d.
- Uses software video decoding.
- Uses FFmpeg libraries for video decoding.
- Uses RomFS to embed media into the application.

Changes to media require rebuilding and reinstalling the application.

## Limitations

- Only Glitch Productions shows are included.
- No automatic episode playback.
- Seeking is keyframe-based.
- Bundled media increases the application's size.