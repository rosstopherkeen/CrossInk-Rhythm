# CrossInk-Rhythm

> A personal fork of [CrossInk](https://github.com/uxjulia/CrossInk) for the **Xteink X4 Pro** that adds two reading stats pages: **Reading Rhythm** and **Books Finished**.

Everything else is stock CrossInk: the same fonts, themes, reader features, settings and data on the SD card. This fork adds two pages to the end of Reading Stats (keeping their data in two new files of its own), its own boot screen, and shows **CrossInk-Rhythm** with the version in Settings. Your existing stats are never changed, so you can flash stock CrossInk back at any time.

<table>
  <tr>
    <td align="center">
      <img src="./docs/images/rhythm/reading-rhythm.png" width="240" alt="Reading Rhythm page: recent activity dot grid, weekly reading time bars, reading days by month" /><br/>
      <em>Reading Rhythm</em>
    </td>
    <td align="center">
      <img src="./docs/images/rhythm/books-finished.png" width="240" alt="Books Finished page: totals card and list of finished books grouped by month" /><br/>
      <em>Books Finished</em>
    </td>
  </tr>
</table>

<sub>Simulator screenshots of the X4 Pro with sample data.</sub>

---

## What this fork adds

### Reading Rhythm

How often and how much you read, at a glance:

- **Recent Activity**: a dot for each day over the last 13 weeks, with bigger, darker dots for longer reading days. Today has a ring around it. Below it, the last 7 days show the time read each day.
- **Weekly Reading Time**: one bar per week for the same 13 weeks. The current week is solid, and a dashed line marks your average.
- **Reading Days by Month**: how many days you read in each of the last 12 months, plus the total and the monthly average.

### Books Finished

- **Totals**: books read, total time spent on finished books, and average time per book.
- **Recently Finished**: each finished book with its title, start → finish date, author and time spent, grouped by month.
- The list is a scroll panel with a scroll bar: swipe up/down to scroll, or move the selection with Up/Down.
- Tap a book (or select it and press Confirm) to open that book's own stats.
- Books that dropped out of Recent Books are still found by scanning the book caches already on the SD card.

<table>
  <tr>
    <td align="center">
      <img src="./docs/images/rhythm/this-device.png" width="200" alt="Existing This Device stats page" /><br/>
      <em>This Device (stock CrossInk)</em>
    </td>
    <td align="center">
      <img src="./docs/images/rhythm/reading-rhythm.png" width="200" alt="Reading Rhythm page" /><br/>
      <em>→ Reading Rhythm</em>
    </td>
    <td align="center">
      <img src="./docs/images/rhythm/books-finished-scrolled.png" width="200" alt="Books Finished list scrolled with a book selected" /><br/>
      <em>→ Books Finished (scrolled, selected)</em>
    </td>
  </tr>
</table>

### Boot screen and version

<img src="./docs/images/rhythm/boot-screen.png" width="160" align="right" alt="CrossInk-Rhythm boot screen" />

The default boot screen is replaced by the CrossInk-Rhythm image above (`src/images/boot-rhythm.bmp`, run-length encoded by `scripts/bmp_to_boot_header.py` to about 22 KB of flash). CrossInk's custom boot screen setting still takes priority when it's on. Settings › System shows the firmware as **CrossInk-Rhythm v&lt;version&gt;**.

<br clear="right"/>

### Where to find them

Open **Reading Stats** the usual way. On the X4 Pro that's the frontlight panel, or the reader menu on button devices. Swipe left or press Right/Down past *This Device* (and *All Devices*, if you sync stats). The two new pages share the existing page dots, header and card layout.

The pages need the device clock. On devices without one, Reading Stats keeps its single-screen layout.

---

## How your data is handled

- **Existing stats are never changed.** `global_stats.bin` stays at version 3 (159 bytes) and each book's `stats_v5.bin` is only read. Stock CrossInk can still read the same card.
- **New data lives in two new files** under `/.crosspoint/`, both versioned and saved safely (temp file, check, `.bak`, rename):
  - `daily_minutes.bin` stores minutes read per day for the last 91 days, recorded at the end of each reading session.
  - `finished_books.bin` is the Books Finished list (up to 32 books), in XPoint's file format.
- **Reading days are real.** They come straight from CrossInk's existing reading history.
- **Earlier minutes are estimated once.** Daily minutes didn't exist before this fork. The first time you open Reading Rhythm, each book's total reading time is spread over the days you actually read it. Estimated days are drawn lighter (outlined dots, light bar segments, `~` before the value). Real minutes recorded from then on always replace the estimates.

File layouts are documented in [docs/file-formats.md](./docs/file-formats.md).

### Going back to stock CrossInk

Flash a CrossInk release. It ignores `daily_minutes.bin` and `finished_books.bin`, and all your existing stats stay readable. To clean up, delete those two files (and any `.bak` copies) from `/.crosspoint/`.

---

## Installation

1. Download `firmware-x4-pro.bin` from this fork's [releases](https://github.com/rosstopherkeen/CrossInk-Rhythm/releases), or build it yourself (see below).
2. Back up `/.crosspoint/` on your SD card.
3. Copy the `.bin` to the SD card and install it from **Settings › System › SD Card Firmware Update**.

Over-the-air updates (**Settings › Check for Updates**) check this fork's releases, not CrossInk's.

**Supported devices:** built and tested on the **Xteink X4 Pro**. The other CrossInk targets (X3/X4, X4 Classic, Sticky) build from the same code but are untested.

---

## Building

PlatformIO builds the firmware. You need **Python 3.10 or newer** and **PlatformIO 6.1.x**. PlatformIO 6.2.0 currently breaks the ESP32-C3 link step with `No module named 'SCons.Tool.FortranCommon'`.

```bash
git submodule update --init --recursive
```

```bash
pio run -e x4-pro
```

The firmware ends up at `.pio/build/x4-pro/firmware.bin`.

Run the host tests, including `test/reading_rhythm/` for the new data files and the estimate math:

```bash
cmake -S test -B build-tests && cmake --build build-tests -j 8 && ctest --test-dir build-tests --output-on-failure
```

Simulator: `pio run -e x4-pro-simulator -t run_simulator`, with an SD card copied into `./fs_/`. The simulator has no clock, so the new pages only appear on real hardware.

The fork's code lives in `src/activities/reader/rhythm/`. The hooks into existing CrossInk code are small: recording minutes when a reading session ends, recording a book when it's marked finished, adding the two pages to the stats screen, and pointing over-the-air updates at this fork. For general CrossInk development docs, see [Getting Started](./docs/development/getting-started.md).

---

## Credits

- **[CrossInk](https://github.com/uxjulia/CrossInk)** by uxjulia, the firmware this fork is built on. For CrossInk's own features (fonts, themes, Focus Reading, button remapping, stats sync and more), see its [README](https://github.com/uxjulia/CrossInk#readme) and [crossink.dev](https://www.crossink.dev). Please support CrossInk directly via its [Ko-fi](https://ko-fi.com/Q5Q01M6S7).
- **[XPoint](https://github.com/Belphemur/XPoint)** (MIT License, Copyright (c) 2025 CrossPoint Reader organization), where the Reading Rhythm and Books Finished screens and the finished-books file format come from: `ReadingRhythmActivity`, `FinishedBooksActivity`, `FinishedBooksIndex`, and `renderReadingRhythmPage` / `renderFinishedBooksPage` from `BookStatsView.cpp`.
- **[CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader)**, the upstream firmware for both.

This is a personal fork. Issues with CrossInk itself belong in the [CrossInk repo](https://github.com/uxjulia/CrossInk); issues with the two stats pages belong [here](https://github.com/rosstopherkeen/CrossInk-Rhythm/issues).
