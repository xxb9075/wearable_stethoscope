"""Cut annotated recordings into breath-phase segments for training.

Each annotated breath phase (exhale, inhale, pause) is saved as a separate
.npy waveform:

    <export_root>/{training,testing}/{exhale,inhale,pause}/<subject>_<pos>_<maneuver>_<label>_id_<n>.npy

Input layout:

    <data_root>/<subject>/<position>/<recording>.csv          respiratory signal, 4 kHz
    <annotation_root>/<subject>/<position>/<recording>.xlsx   phase annotations

The annotation sheet has the columns subject_id, position, maneuver, label,
start_time and end_time (seconds). Segments of the held-out test subjects and
all segments of the "irregular" maneuver go to the testing set; everything else
goes to the training set.

Example, run from the source_separation_classification folder:

    python src/frameset_creation.py --data_root data/fully_prepared --annotation_root data/annotations --export_root data/framesets --test_subjects S08 S09
"""

import argparse
import os
from pathlib import Path
import shutil

import numpy as np
import pandas as pd


def rm_all_files_w_name(name, root='.'):
    # Remove all files and folders with the given name below root
    # (used for macOS .DS_Store files, which would be listed as subjects).
    for path in Path(root).rglob(name):
        if path.is_dir():
            shutil.rmtree(path)
            print("Deleted directory:", path)
        elif path.is_file():
            path.unlink()
            print("Deleted file:", path)


def frameset_creation(dataRoot="data/fully_prepared", annotationRoot="data/annotations",
                      exportRoot="data/framesets", testSubjects=(), subjects=None):
    fs = 4000

    framesets = ["training", "testing"]
    breathTypes = ["exhale", "inhale", "pause"]

    for frameset in framesets:
        for breathType in breathTypes:
            directory = os.path.join(exportRoot, frameset, breathType)
            if not os.path.exists(directory):
                os.makedirs(directory)
                print(f"Directory {directory} created.")

    # Remove .DS_Store files from the input folders
    rm_all_files_w_name(".DS_Store", dataRoot)
    rm_all_files_w_name(".DS_Store", annotationRoot)

    if subjects is None:
        subjects = os.listdir(dataRoot)

    print("\nBeginning processing:\n====================\n")

    for subject in subjects:
        subjectPath = os.path.join(dataRoot, subject)
        print(f"Subject: {subject}")

        for position in os.listdir(subjectPath):
            positionPath = os.path.join(subjectPath, position)
            print(f"Position: {position}")

            filename = os.listdir(positionPath)  # pick the .csv file (not the .wav)
            if (filename[0])[-3:] == 'csv': filename = filename[0]
            else: filename = filename[1]

            dataPath = os.path.join(positionPath, filename)
            annotationPath = os.path.join(annotationRoot, subject, position, filename[:-4] + ".xlsx")

            # Read data and annotations
            annotations = pd.read_excel(annotationPath, header=0)
            data = np.genfromtxt(dataPath, delimiter=",")

            for i in range(annotations.shape[0]):  # one row per breath phase
                label = annotations.at[i, 'label']
                maneuver = annotations.at[i, 'maneuver']

                exportName = "_".join([annotations.at[0, 'subject_id'], annotations.at[i, 'position'][0:3], maneuver, label[0:2], "id", str(i)]) + ".npy"

                startInd = int(fs*annotations.at[i, 'start_time'])
                endInd = int(fs*annotations.at[i, 'end_time']+1)
                if endInd <= startInd:  # skip segments with zero length
                    print("Skipping breath segment. Start and end time are not consistent.")
                    continue

                slice = data[startInd:endInd]

                if maneuver == 'irregular' or subject in testSubjects:
                    setType = 'testing'
                else:
                    setType = 'training'

                np.save(os.path.join(exportRoot, setType, label, exportName), slice)

            print(f"Finished processing breaths for {position}.")
        print(f"Finished processing breaths for {subject}.\n====================\n")


def main():
    parser = argparse.ArgumentParser(description="Create breath-phase framesets from annotated recordings.")
    parser.add_argument("--data_root", default="data/fully_prepared", help="folder with <subject>/<position>/<recording>.csv")
    parser.add_argument("--annotation_root", default="data/annotations", help="folder with <subject>/<position>/<recording>.xlsx")
    parser.add_argument("--export_root", default="data/framesets", help="output folder")
    parser.add_argument("--test_subjects", nargs="*", default=[], help="subject folders held out for testing")
    args = parser.parse_args()

    frameset_creation(args.data_root, args.annotation_root, args.export_root, args.test_subjects)


if __name__ == '__main__':
    main()
