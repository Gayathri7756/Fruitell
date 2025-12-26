FRUITELL - Smart Fruit Freshness Detection
Overview
A portable IoT device to assess fruit freshness non-destructively using ultrasonic sensors. Python ML models classify freshness levels in real-time and store results in CSV/Excel files for analysis.
Technologies
Python (ML models for classification)
Arduino (embedded system for sensors)
CSV/Excel for data logging

Project Structure
Fruitell/ ├── fruitell_program_from_csv.py
├── fruitell_train.py
├── testconnect.py
├── test-port.py
├── fruitell-sketch/
│   └── fruitell-sketch.ino
├── runs/
│   ├── anchors_update.csv
│   ├── sample_session.csv
│   ├── session1.csv
│   └── smoke_test.csv
└── README.md


How to Run

1.Clone the repo:
git clone https://github.com/YourUsername/Fruitell.git

2.Navigate to folder and create virtual environment:
cd Fruitell
python -m venv venv

3.Activate environment and install dependencies:
venv\Scripts\activate   # Windows
source venv/bin/activate # Mac/Linux
pip install -r requirements.txt

4.Run training or update commands:
# Training the device
python fruitell_train.py --port COM8 --out runs/sample_session.csv --min-conf 0

# Updating anchors
python fruitell_program_from_csv.py --port COM6 --baud 115200 --csv runs/anchors_update.csv

# Uploading CSV to Arduino
python fruitell_program_from_csv.py --port COM6 --csv runs/sample_session.csv --per_line_delay 0.15

Arduino Serial Monitor Commands
R → print anchors, TRAINED, ACCUM_MODE, TOTAL fresh/spoil counts
F / S → save Fresh / Spoiled anchor from live median
MODEL:RESET → clears TRAINED flag and totals
TRAIN:ON / TRAIN:OFF → live CSV streaming
SNAP → one CSV line (for testing)
CSVTEST:BEGIN … CSVTEST:END → send rows for training
CSVACCUM:ON / CSVACCUM:OFF → accumulate or replace totals
CSVACCUM:CLEAR → clear totals while keeping anchors

TFLAG? / TFLAG:0 / TFLAG:1 → view or set TRAINED flag
