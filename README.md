# FRUITELL - Smart Fruit Freshness Detection

## Overview
FRUITELL is a portable IoT device to assess fruit freshness non-destructively using ultrasonic sensors. Python ML models classify freshness levels in real-time and store results in CSV/Excel files for analysis.

## Technologies
- Python (ML models for classification)  
- Arduino (embedded system for sensors)  
- CSV for data logging  

## Project Structure

Fruitell/ ├── fruitell_program_from_csv.py 
          ├── fruitell_train.py 
          ├── testconnect.py 
          ├── test-port.py 
          ├── fruitell-sketch/ 
          │   └── fruitell-sketch.ino 
          ├── runs/ 
          │   └── sample_session.csv 
          └── README.md

## How to Run

### Record Training Data and Upload to Arduino
```bash
# Record training data
python fruitell_train.py --port COM5 --out runs/sample_session.csv --min-conf 0

# Upload recorded CSV to Arduino
python fruitell_program_from_csv.py --port COM5 --csv runs/sample_session.csv --per_line_delay 0.15

# Arduino Serial Monitor Commands (Cheat-sheet)

R → Print anchors, TRAINED, ACCUM_MODE, TOTAL fresh/spoil counts

F / S → Save Fresh / Spoiled anchor from live median

MODEL:RESET → Clears TRAINED flag and totals

TRAIN:ON / TRAIN:OFF → Live CSV streaming

SNAP → One CSV line (+ human line if trained)

CSVTEST:BEGIN … CSVTEST:END → Send rows for training

CSVACCUM:ON / CSVACCUM:OFF → Set accumulate mode

CSVACCUM:CLEAR → Clear totals while keeping anchors

TFLAG? / TFLAG:0 / TFLAG:1 → View or set TRAINED flag
