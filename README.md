### WAV conversion script (`scripts/wav2speech.py`)

Converts WAV files (any bit depth, stereo/mono, PCM or float format) to the `speech.raw` cache format:
```
python3 scripts/wav2speech.py input.wav [-v VERSION] [-r SAMPLE_RATE] [-o OUTPUT]
```
- Defaults: `--version 12`, `--resample 16000`, `--output data/speech.raw`
- Handles PCM (format 1) and IEEE float (format 3) WAV files
- Resamples to target rate via linear interpolation

Run from the pioarduino terminal.

### SPIFFS Upload Command

Output is written to data/speech.raw, ready for SPIFFS upload via:
    pio run --target uploadfs