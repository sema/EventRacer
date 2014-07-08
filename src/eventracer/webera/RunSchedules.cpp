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
#include <iostream>
#include <vector>
#include <tr1/memory>
#include <tr1/shared_ptr.h>

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

DEFINE_string(tmp_er_log_file, "/tmp/new_er_log",
        "Filename with the new ER log.");
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

bool MoveFile(const std::string& file, const std::string& out_dir) {
	if (system(StringPrintf("mv %s %s", file.c_str(), out_dir.c_str()).c_str()) != 0) {
        fprintf(stderr, "Cannot move %s\n", file.c_str());
        return false;
	}
	return true;
}

bool performSavedSchedule(const std::string& schedule_name, std::string* executed_schedule_log, std::string* executed_er_log) {
    *executed_schedule_log = "";
    *executed_er_log = "";

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
    if (!MoveFile(FLAGS_tmp_er_log_file, out_dir)) return false;
	if (!MoveFile(FLAGS_tmp_schedule_file, out_dir)) return false;
	if (!MoveFile(FLAGS_tmp_png_file, out_dir)) return false;
	if (!MoveFile(FLAGS_tmp_error_log, out_dir)) return false;
	if (!MoveFile(FLAGS_tmp_stdout, out_dir)) return false;

    size_t schedule_pos = FLAGS_tmp_schedule_file.find_last_of('/');
    if (schedule_pos == std::string::npos) {
        schedule_pos = 0;
    }

    size_t log_pos = FLAGS_tmp_er_log_file.find_last_of('/');
    if (log_pos == std::string::npos) {
        log_pos = 0;
    }

    *executed_schedule_log = StringPrintf("%s/%s", out_dir.c_str(), FLAGS_tmp_schedule_file.substr(schedule_pos).c_str());
    *executed_er_log = StringPrintf("%s/%s", out_dir.c_str(), FLAGS_tmp_er_log_file.substr(log_pos).c_str());

	return true;
}

typedef std::vector<int> Schedule;

typedef struct {

    std::string name;
    std::vector<std::pair<int, Schedule> > EAT;
    std::set<int> visited;
    int depth;
    Schedule schedule;
    std::tr1::shared_ptr<TraceReorder> reorder;

} State;

bool StateHasUnexploredEAT(const State& state, Schedule* schedule, int* race_id) {

    schedule->clear();
    *race_id = -1;

    for (int i = state.EAT.size()-1; i >= 0; --i) {
        const std::pair<int, Schedule>& entry = state.EAT[i];
        if (state.visited.find(entry.second[0]) == state.visited.end()) {
            *schedule = entry.second;
            *race_id = entry.first;
            return true;
        }
    }

    return false;
}

void EATMerge(std::vector<State>* stack, size_t offset, const Schedule& schedule, int race_id) {

    /*
     * Notice, the offset references the position in on the stack on which the schedule
     * should be rooted (or merged into)
     *
     * Thus, schedule[0+offset] references the first event, which should be rooted in stack[0+offset] and leads
     * to stack[1+offset].
     */

    size_t schedule_offset = 0;

    // next element on stack exist and next element matches next element in the schedule
    while (stack->size() > offset+1 &&
           schedule.size() > schedule_offset &&
           stack->at(offset+1).schedule.back() == schedule[schedule_offset]) {

        ++offset;
        ++schedule_offset;
    }

    if (schedule.size() <= schedule_offset) {
        // The schedule have already been explored
        return;
    }

    std::vector<int> sufix(schedule.begin()+schedule_offset, schedule.end());
    stack->at(offset).EAT.push_back(std::pair<int, Schedule>(race_id, sufix));

}

void explore(const char* initial_schedule, const char* initial_er_log) {

    int all_schedules = 0;
    int successful_reverses = 0;
    int successful_schedules = 0;

    std::vector<State> stack;

    // Initialize

    std::tr1::shared_ptr<TraceReorder> init_reorder = std::tr1::shared_ptr<TraceReorder>(new TraceReorder());
    init_reorder->LoadSchedule(initial_schedule);

    State initial_state;
    initial_state.depth = -1;
    initial_state.name = "base";
    initial_state.reorder = init_reorder;
    initial_state.EAT.push_back(std::pair<int, Schedule>(-1, init_reorder->GetSchedule()));

    stack.push_back(initial_state);

    while (!stack.empty()) {

        State state = stack.back();

        /*
         * Check for unexplored schedule, follow it, and update EATs
         * Step 4 - (1 - 2)* - 3
         */

        Schedule new_schedule;
        int new_race_id;
        if (StateHasUnexploredEAT(state, &new_schedule, &new_race_id)) {

            // Step 4 -- Execute new schedule

            ++successful_reverses;

            std::string new_name = StringPrintf("%s_race%d", state.name.c_str(), new_race_id);
            fprintf(stderr, "Reordering \"%s\": ", new_name.c_str());

            state.reorder->SaveSchedule(FLAGS_tmp_schedule_file.c_str(), new_schedule);

            std::string executed_schedule_log;
            std::string executed_er_log;
            if (performSavedSchedule(new_name, &executed_schedule_log, &executed_er_log)) {

                ++successful_schedules;

                std::tr1::shared_ptr<TraceReorder> new_reorder = std::tr1::shared_ptr<TraceReorder>(new TraceReorder());
                new_reorder->LoadSchedule(executed_schedule_log.c_str());
                RaceApp new_race_app(0, executed_er_log, false);

                // Step 1 & Step 2 -- push states

                Schedule executed_schedule = new_reorder->GetSchedule();
                size_t old_state_index = stack.size() - 1;

                int new_depth = state.depth + 1;

                // The stack should be a prefix of executed_schedule.
                for (size_t i = stack.size(); i < executed_schedule.size(); ++i) {
                    State new_state;
                    new_state.depth = new_depth;
                    new_state.name = new_name;

                    new_state.schedule = state.schedule;
                    new_state.schedule.push_back(executed_schedule[i]);

                    new_state.reorder = new_reorder;

                    state.visited.insert(executed_schedule[i]);

                    stack.push_back(new_state);
                    state = stack.back();
                }

                State& old_state = stack[old_state_index];
                std::vector<std::pair<int, Schedule> > old_eat;
                old_eat.swap(old_state.EAT);

                while (!old_eat.empty()) {
                    std::pair<int, Schedule> eat = old_eat.back();
                    old_eat.pop_back();

                    EATMerge(&stack, old_state_index, eat.second, eat.first);
                }

                // Step 3 -- update EATs

                TraceReorder::Options options;
                options.include_change_marker = true;
                options.relax_replay_after_all_races = true;

                const VarsInfo& vinfo = new_race_app.vinfo();
                //const EventGraphInterface* hb = vinfo.fast_event_graph();
                std::map<int, std::map<int, std::vector<int> > > raceMap; // <event1, event2> => [race ...]

                // Build raceMap for quick "does x and y race" lookups
                for (size_t race_id = 0; race_id < vinfo.races().size(); ++race_id) {

                    const VarsInfo::RaceInfo& race = vinfo.races()[race_id];

                    // Ignore covered races
                    if (!race.m_multiParentRaces.empty() || race.m_coveredBy != -1) continue;

                    std::map<int, std::vector<int> >& racing = raceMap[race.m_event2];
                    std::vector<int>& races = racing[race.m_event1];
                    races.push_back(race_id);
                }

                std::cout << "Checking for races in event sequence of length " << state.schedule.size() << std::endl;

                for (int i = state.schedule.size() - 1; i >= 0; --i) {

                    int event2 = state.schedule[i];
                    std::map<int, std::vector<int> >& racing = raceMap[event2];

                    if (racing.size() == 0) {
                        continue;
                    }

                    for (int j = i - 1; j >= 0; --j) {

                        int event1 = state.schedule[j];
                        std::vector<int>& races = racing[event1];

                        if (!races.empty()) {

                            ++all_schedules;

                            int race_id = races.front();

                            std::vector<int> pending_schedule;
                            new_reorder->GetScheduleFromRace(vinfo, race_id, new_race_app.graph(), options, &pending_schedule);

                            EATMerge(&stack, 0, pending_schedule, race_id);

                            std::cout << "Identified race between " << i << " - " << event1 << " and " << j << " - " << event2 << " with race #" << race_id << std::endl;

                            break;
                        }
                    }

                }

                std::cout << "Finished iteration" << std::endl;

            } // End successful execution

        } // End if has unexplored EAT

        // backtrack
        stack.pop_back();

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

    explore(FLAGS_in_schedule_file.c_str(), argv[1]);

	return 0;
}
