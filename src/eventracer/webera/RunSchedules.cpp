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

DEFINE_string(in_dir, "/tmp/",
        "Path to dir containing initial log files such as log.network.data");
DEFINE_string(in_schedule_file, "/tmp/schedule.data",
		"Filename with the schedules.");

DEFINE_string(site, "", "The website to replay");

DEFINE_string(replay_command, "LD_LIBRARY_PATH=/home/veselin/gitwk/WebERA/WebKitBuild/Release/lib /home/veselin/gitwk/WebERA/R5/clients/Replay/bin/replay %s %s -in_dir %s",
		"Command to run replay with twice %s for the site and the replay file.");

DEFINE_string(tmp_new_schedule_file, "/tmp/new_schedule.data",
        "Filename with the new schedule to be executed.");

DEFINE_string(tmp_er_log_file, "/tmp/out.ER_actionlog",
        "Filename with the new ER log.");
DEFINE_string(tmp_schedule_file, "/tmp/out.schedule.data",
		"Filename with the schedules.");
DEFINE_string(tmp_error_log, "/tmp/out.errors.log",
		"Filename with the schedules.");
DEFINE_string(tmp_png_file, "/tmp/out.screenshot.png",
		"Filename with the schedules.");
DEFINE_string(tmp_stdout, "/tmp/stdout.txt",
        "Standard output redirect of WebERA.");
DEFINE_string(tmp_network_log, "/tmp/log.network.data",
        "Filename with network data.");
DEFINE_string(tmp_time_log, "/tmp/log.time.data",
        "Filename with time data.");
DEFINE_string(tmp_random_log, "/tmp/log.random.data",
        "Filename with random data.");
DEFINE_string(tmp_status_log, "/tmp/status.data",
        "Filename with status data.");


DEFINE_string(out_dir, "/tmp/outdir",
        "Location of the output.");

DEFINE_int32(conflict_reversal_bound, 1,
        "Conflict-reversal bound.");

namespace {

bool MoveFile(const std::string& file, const std::string& out_dir) {
	if (system(StringPrintf("mv %s %s", file.c_str(), out_dir.c_str()).c_str()) != 0) {
        fprintf(stderr, "Cannot move %s\n", file.c_str());
        return false;
	}
	return true;
}

bool performSchedule(const std::string& race_name, const std::string& base_dir, const std::string& schedule,
                     std::string* executed_race_dir, std::string* executed_schedule_log, std::string* executed_er_log) {

    /*
     * Executes replay command supplying the base dir with recorded data, URL, and path to the schedule.
     *
     * The function _moves_ all known output from the replay command to a race dir, and returns
     * - The path to the race dir
     * - The path to the executed schedule
     * - The path to the ER action log
     */


    *executed_race_dir = "";
    *executed_schedule_log = "";
    *executed_er_log = "";

    std::string command;
	StringAppendF(&command, FLAGS_replay_command.c_str(),
            base_dir.c_str(), FLAGS_site.c_str(), schedule.c_str());
	command += " > ";
	command += FLAGS_tmp_stdout;

    fprintf(stdout, "Running %s\n", command.c_str());

    if (system(command.c_str()) != 0) {
		fprintf(stderr, "Could not run command: %s\n", command.c_str());
		return false;
	}

    std::string out_dir = StringPrintf("%s/%s", FLAGS_out_dir.c_str(), race_name.c_str());

    // Move result files

	if (system(StringPrintf("mkdir -p %s", out_dir.c_str()).c_str()) != 0) {
		fprintf(stderr, "Could not create output dir %s. Set the flag --out_dir\n", out_dir.c_str());
		return false;
	}
    if (!MoveFile(schedule, out_dir)) return false;
    if (!MoveFile(FLAGS_tmp_er_log_file, out_dir + "/ER_actionlog")) return false;
    if (!MoveFile(FLAGS_tmp_schedule_file, out_dir + "/schedule.data")) return false;
    if (!MoveFile(FLAGS_tmp_png_file, out_dir + "/screenshot.png")) return false;
    if (!MoveFile(FLAGS_tmp_error_log, out_dir + "/errors.log")) return false;
    if (!MoveFile(FLAGS_tmp_stdout, out_dir + "/stdout")) return false;
    if (!MoveFile(FLAGS_tmp_network_log, out_dir + "/log.network.data")) return false;
    if (!MoveFile(FLAGS_tmp_time_log, out_dir + "/log.time.data")) return false;
    if (!MoveFile(FLAGS_tmp_random_log, out_dir + "/log.random.data")) return false;
    if (!MoveFile(FLAGS_tmp_status_log, out_dir + "/status.data")) return false;

    // Output

    *executed_race_dir = out_dir;
    *executed_schedule_log = StringPrintf("%s/%s", out_dir.c_str(), "schedule.data");
    *executed_er_log = StringPrintf("%s/%s", out_dir.c_str(), "ER_actionlog");

	return true;
}

typedef int RaceID;
typedef int MixedEventId;
typedef size_t StrictEventID; // no placeholders or -1 error messages

typedef std::vector<StrictEventID> Schedule;
typedef std::vector<StrictEventID> ScheduleSuffix;
typedef std::vector<MixedEventId> ExecutableSchedule; // allow for -1 and -2 markers

typedef struct EATEntry_t {

    EATEntry_t(std::string base_race_output_dir,
               RaceID race_id,
               ScheduleSuffix schedule_suffix,
               ExecutableSchedule executable_schedule,
               std::tr1::shared_ptr<TraceReorder> reorder)
        : base_race_output_dir(base_race_output_dir)
        , race_id(race_id)
        , schedule_suffix(schedule_suffix)
        , executable_schedule(executable_schedule)
        , reorder(reorder)
    {
    }

    std::string base_race_output_dir;
    RaceID race_id;
    ScheduleSuffix schedule_suffix;
    ExecutableSchedule executable_schedule;
    std::tr1::shared_ptr<TraceReorder> reorder;

} EATEntry;

typedef std::vector<EATEntry> EAT;

typedef struct {

    std::string name;

    EAT eat;
    int depth;
    std::set<StrictEventID> visited;
    Schedule schedule;

} State;

bool StateHasUnexploredEAT(const State* state, const EATEntry** result) {

    if (state->depth >= FLAGS_conflict_reversal_bound) {
        return false;
    }

    for (size_t i = 0; i < state->eat.size(); ++i) {
        const EATEntry& entry = state->eat[i];

        if (state->visited.find(entry.schedule_suffix[0]) == state->visited.end()) {
            *result = &entry;
            return true;
        }
    }

    return false;
}

int EATMerge(std::vector<State*>* stack, size_t offset, const EATEntry& entry) {

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
           entry.schedule_suffix.size() > schedule_offset &&
           stack->at(offset+1)->schedule.back() == entry.schedule_suffix[schedule_offset]) {

        ++offset;
        ++schedule_offset;
    }

    if (entry.schedule_suffix.size() <= schedule_offset) {
        // The schedule have already been explored
        return -1;
    }

    EATEntry new_entry = entry;
    new_entry.schedule_suffix = ScheduleSuffix(entry.schedule_suffix.begin()+schedule_offset, entry.schedule_suffix.end());

    stack->at(offset)->eat.push_back(new_entry);

    return schedule_offset;

}

void EATPropagate(std::vector<State*>* stack, size_t index) {

    State* state = stack->at(index);
    EAT old_eat;
    old_eat.swap(state->eat);

    while (!old_eat.empty()) {
        EATEntry eat = old_eat.back();
        old_eat.pop_back();

        EATMerge(stack, index, eat);
    }
}

void explore(const char* initial_schedule, const char* initial_base_dir) {

    int all_schedules = 0;
    int successful_reverses = 0;
    int successful_schedules = 0;

    std::vector<State*> stack;
    stack.reserve(5000);

    // Initialize

    std::tr1::shared_ptr<TraceReorder> init_reorder = std::tr1::shared_ptr<TraceReorder>(new TraceReorder());
    init_reorder->LoadSchedule(initial_schedule);

    ExecutableSchedule init_executable_schedule = init_reorder->GetSchedule();
    Schedule init_schedule = init_reorder->RemoveSpecialMarkers(init_executable_schedule);

    EATEntry init_eat(initial_base_dir, -1, init_schedule, init_executable_schedule, init_reorder);

    State* initial_state = new State();
    initial_state->depth = -1;
    initial_state->name = "";
    initial_state->eat.push_back(init_eat);

    stack.push_back(initial_state);

    while (!stack.empty()) {

        State* state = stack.back();

        /*
         * Check for unexplored schedule, follow it, and update EATs
         * Step 4 - (1 - 2)* - 3
         */

        const EATEntry* next_eat = NULL;
        if (StateHasUnexploredEAT(state, &next_eat) && next_eat != NULL) {

            // Step 4 -- Execute new schedule

            ++successful_reverses;

            std::string new_name = next_eat->race_id == -1 ? "base" : StringPrintf("%s_race%d", state->name.c_str(), next_eat->race_id);
            fprintf(stderr, "Reordering \"%s\" at depth %d (limit %d): ", new_name.c_str(), state->depth, FLAGS_conflict_reversal_bound);

            next_eat->reorder->SaveSchedule(FLAGS_tmp_new_schedule_file.c_str(), next_eat->executable_schedule);

            std::string executed_base_dir;
            std::string executed_schedule_log;
            std::string executed_er_log;

            state->visited.insert(next_eat->schedule_suffix[0]);

            if (performSchedule(new_name, next_eat->base_race_output_dir, FLAGS_tmp_new_schedule_file.c_str(),
                                &executed_base_dir, &executed_schedule_log, &executed_er_log)) {

                ++successful_schedules;

                std::tr1::shared_ptr<TraceReorder> new_reorder = std::tr1::shared_ptr<TraceReorder>(new TraceReorder());
                new_reorder->LoadSchedule(executed_schedule_log.c_str());
                RaceApp new_race_app(0, executed_er_log, false);

                // Step 1 & Step 2 -- push states

                ExecutableSchedule executed_schedule = new_reorder->GetSchedule();
                Schedule schedule = new_reorder->RemoveSpecialMarkers(executed_schedule);

                size_t old_state_index = stack.size() - 1;

                int new_depth = state->depth + 1;

                // The stack should be a prefix of executed_schedule (with an empty zero element).
                for (size_t i = stack.size()-1; i < schedule.size(); ++i) {
                    State* new_state = new State();
                    new_state->depth = new_depth;
                    new_state->name = new_name;

                    new_state->schedule = state->schedule;
                    new_state->schedule.push_back(schedule[i]);

                    state->visited.insert(schedule[i]);

                    stack.push_back(new_state);
                    state = stack.back();
                }

                EATPropagate(&stack, old_state_index);

                // Step 3 -- update EATs

                TraceReorder::Options options;
                options.include_change_marker = true;
                options.relax_replay_after_all_races = true;

                const VarsInfo& vinfo = new_race_app.vinfo();

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

                for (int i = state->schedule.size() - 1; i >= 0; --i) {

                    if (stack[i]->depth != state->depth) {
                        break; // optimization, we don't need to re-check trees we have already explored.
                    }

                    int event2 = state->schedule[i];
                    std::map<int, std::vector<int> >& racing = raceMap[event2];

                    if (racing.size() == 0) {
                        continue;
                    }

                    for (int j = i - 1; j >= 0; --j) {

                        int event1 = state->schedule[j];
                        std::vector<int>& races = racing[event1];

                        if (!races.empty()) {

                            ++all_schedules;

                            RaceID race_id = races.front();

                            ExecutableSchedule pending_executable_schedule;
                            new_reorder->GetScheduleFromRace(vinfo, race_id, new_race_app.graph(), options, &pending_executable_schedule);
                            ScheduleSuffix pending_schedule = new_reorder->RemoveSpecialMarkers(pending_executable_schedule);
                            EATEntry pending_entry(executed_base_dir, race_id, pending_schedule, pending_executable_schedule, new_reorder);

                            EATMerge(&stack, 0, pending_entry);

                            break;
                        }
                    }

                }

            } // End successful execution

        } else { // End if has unexplored EAT

            // backtrack

            delete state;

            state = stack.back();
            stack.pop_back();

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

    explore(FLAGS_in_schedule_file.c_str(), FLAGS_in_dir.c_str());

	return 0;
}
