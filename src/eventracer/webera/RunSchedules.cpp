/*
   Copyright 2014 Software Reliability Lab, ETH Zurich

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vector>

#include "gflags/gflags.h"

#include "RaceApp.h"
#include "stringprintf.h"
#include "strutil.h"
#include "TraceReorder.h"

DEFINE_string(in_schedule_file, "/tmp/schedule.data",
		"Filename with the schedules.");

DEFINE_string(site, "", "The website to replay");

DEFINE_string(replay_command, "LD_LIBRARY_PATH=/home/veselin/gitwk/WebERA/WebKitBuild/Release/lib /home/veselin/gitwk/WebERA/R5/clients/Replay/bin/replay %s %s",
		"Command to run replay with twice %s for the site and the replay file.");

DEFINE_string(tmp_schedule_file, "/tmp/new_schedule.data",
		"Filename with the schedules.");
DEFINE_string(tmp_error_log, "/tmp/errors.log",
		"Filename with the schedules.");
DEFINE_string(tmp_png_file, "/tmp/replay.png",
		"Filename with the schedules.");
DEFINE_string(tmp_stdout, "/tmp/stdout.txt", "Standard output redirect of WebERA.");

DEFINE_string(out_dir, "/tmp/outdir", "Location of the output.");

DEFINE_int32(max_races_per_memory_location, 3,
		"Maximum number of races to try to reverse per memory location.");

namespace {

RaceApp* race_app;
TraceReorder* reorder;


bool MoveFile(const std::string& file, const std::string& out_dir) {
	if (system(StringPrintf("mv %s %s", file.c_str(), out_dir.c_str()).c_str()) != 0) {
        fprintf(stderr, "Cannot move %s\n", file.c_str());
        return false;
	}
	return true;
}

bool performSavedSchedule(const std::string& schedule_name) {
	std::string command;
	StringAppendF(&command, FLAGS_replay_command.c_str(),
			FLAGS_site.c_str(), FLAGS_tmp_schedule_file.c_str());
	command += " > ";
	command += FLAGS_tmp_stdout;
	if (system(command.c_str()) != 0) {
		fprintf(stderr, "Could not run command: %s\n", command.c_str());
		return false;
	}

	std::string out_dir = StringPrintf("%s/%s", FLAGS_out_dir.c_str(), schedule_name.c_str());

	if (system(StringPrintf("mkdir -p %s", out_dir.c_str()).c_str()) != 0) {
		fprintf(stderr, "Could not create output dir %s. Set the flag --out_dir\n", out_dir.c_str());
		return false;
	}
	if (!MoveFile(FLAGS_tmp_schedule_file, out_dir)) return false;
	if (!MoveFile(FLAGS_tmp_png_file, out_dir)) return false;
	if (!MoveFile(FLAGS_tmp_error_log, out_dir)) return false;
	if (!MoveFile(FLAGS_tmp_stdout, out_dir)) return false;

	return true;
}


void createReorders() {
	const VarsInfo& vinfo = race_app->vinfo();

	TraceReorder::Options options;
	options.include_change_marker = true;
	options.relax_replay_after_all_races = true;

	int all_schedules = 0;
	int successful_reverses = 0;
	int successful_schedules = 0;

	std::map<int, int> reversals_per_memory_locations;
	for (int race_id = -1; race_id < static_cast<int>(vinfo.races().size()); ++race_id) {

        std::string name;
        std::vector<int> new_schedule;

        ++all_schedules;

        if (race_id == -1) {
			// Do not reverse any races in this case.
			name = "base";
            reorder->GetScheduleWithoutRace(&new_schedule);

		} else {
			name = StringPrintf("race%d", race_id);
			const VarsInfo::RaceInfo& race = vinfo.races()[race_id];
			if (!race.m_multiParentRaces.empty() || race.m_coveredBy != -1) continue;
			if (reversals_per_memory_locations[race.m_varId]++ >= FLAGS_max_races_per_memory_location) {
                --all_schedules; // don't count this
				continue;
            }

            // Reverse the current race (and only it).
            if (reorder->GetScheduleFromRace(vinfo, race_id, race_app->graph(), options, &new_schedule)) {
                ++successful_reverses;
            } else {
                continue;
            }
		}

		fprintf(stderr, "Reordering \"%s\": ", name.c_str());

        reorder->SaveSchedule(FLAGS_tmp_schedule_file.c_str(), new_schedule);
        if (performSavedSchedule(name)) {
            ++successful_schedules;
        }
	}

	printf("Tried %d schedules. %d generated, %d successful\n",
			all_schedules, successful_reverses, successful_schedules);
}

}  // namespace


int main(int argc, char* argv[]) {
	google::ParseCommandLineFlags(&argc, &argv, true);

	if (FLAGS_site.empty()) {
		fprintf(stderr, "  --site is a mandatory parameter.\n");
		return -1;
	}

	reorder = new TraceReorder();
	reorder->LoadSchedule(FLAGS_in_schedule_file.c_str());
	race_app = new RaceApp(0, argv[1], false);

	createReorders();

	return 0;
}
