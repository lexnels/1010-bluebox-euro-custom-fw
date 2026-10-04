# 1010-bluebox-euro-custom-fw

Custom firmware modifications for the 1010music **bluebox eurorack edition**
(not the desktop bluebox; firmware images differ between the two).

## Goals

- Add a **CPU meter** to the UI.
- Add a new **Room reverb** algorithm.

## Approach

Follows the approach of [j3threejay/blackbox-mod](https://github.com/j3threejay/blackbox-mod):
patching the stock firmware image rather than rebuilding from source.

## Note on firmware images

The stock firmware is 1010music's property and is **not** included in this repo.
`*.bin` / `*.BIN` files are git-ignored. Download the official eurorack-edition
firmware from 1010music and keep it locally.
