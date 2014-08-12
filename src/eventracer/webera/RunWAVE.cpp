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
#include <limits>
#include <algorithm>
#include <sstream>

#include "gflags/gflags.h"

#include "RaceApp.h"
#include "stringprintf.h"
#include "strutil.h"
#include "TraceReorder.h"

DEFINE_string(in_dir, "",
        "Path to dir containing initial log files such as log.network.data");
DEFINE_string(in_schedule_file, "",
		"Filename with the schedules.");

DEFINE_string(site, "", "The website to replay");

DEFINE_string(replay_command, "/home/veselin/gitwk/WebERA/R5/clients/Replay/bin/replay %s %s -in_dir %s",
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

DEFINE_int32(iteration_bound, 1,
        "Maximum number of iterations.");

namespace {

bool MoveFile(const std::string& file, const std::string& out_dir) {
	if (system(StringPrintf("mv %s %s", file.c_str(), out_dir.c_str()).c_str()) != 0) {
        fprintf(stderr, "Cannot move %s\n", file.c_str());
        return false;
	}
	return true;
}

bool performSchedule(const std::string& race_name, const std::string& origin, const std::string& base_dir, const std::string& schedule,
                     std::string* executed_race_dir, std::string* executed_schedule_log, std::string* executed_er_log) {

    /*
     * Executes replay command supplying the base dir with recorded data, URL, and path to the schedule.
     *
     * The function _moves_ all known output from the replay command to a race dir, and returns
     * - The path to the race dir
     * - The path to the executed schedule
     * - The path to the ER action log
     */

    setbuf(stdout, NULL);

    *executed_race_dir = "";
    *executed_schedule_log = "";
    *executed_er_log = "";

    std::string command;
	StringAppendF(&command, FLAGS_replay_command.c_str(),
            base_dir.c_str(), FLAGS_site.c_str(), schedule.c_str());
	command += " > ";
	command += FLAGS_tmp_stdout;

    printf("Running %s\n", command.c_str());

    if (system(command.c_str()) != 0) {
        printf("Could not run command: %s\n", command.c_str());
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

    if (system(StringPrintf("echo \"%s\" > %s/origin", origin.c_str(), out_dir.c_str()).c_str()) != 0) {
        fprintf(stderr, "Could not create origin file\n");
        return false;
    }

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
typedef std::vector<MixedEventId> ExecutableSchedule; // allow for -1 and -2 markers

typedef std::vector<StrictEventID> EventQueue;

typedef struct state_t {

    state_t(size_t init)
        : nextToBeExecuted(init)
    {
    }

    size_t nextToBeExecuted;

} State;

bool reverse_schedule(const EventGraphInterface* hb, Schedule* schedule, StrictEventID event1, StrictEventID event2) {

    Schedule old_schedule;
    old_schedule.swap(*schedule);

    schedule->reserve(old_schedule.size());

    /*
     * Takes a schedule on the form axbyc, where a b and c are event sequences and
     * x and y are events we want to swap (event1, event2).
     *
     * This algorithm reorders ``schedule``, reversing x and y. The resulting
     * schedule is on the form ab'yxb''c, where b' is the sequence of events in b
     * which do not happen after x, and b'' is the sequence of events which happen after x.
     */

    // Emit ``a`` until we see x

    size_t schedule_pos = 0;

    for (; schedule_pos < old_schedule.size() && old_schedule[schedule_pos] != event1; ++schedule_pos) {
        schedule->push_back(old_schedule[schedule_pos]);
    }

    // Skip x

    std::vector<StrictEventID> bprimeprime;

    bprimeprime.push_back(old_schedule[schedule_pos]);
    ++schedule_pos;

    // Emit b' until we see y

    for (; schedule_pos < old_schedule.size() && old_schedule[schedule_pos] != event2; ++schedule_pos) {

        if (hb->areOrdered(event1, old_schedule[schedule_pos])) {
            bprimeprime.push_back(old_schedule[schedule_pos]);
        } else {
            schedule->push_back(old_schedule[schedule_pos]);
        }
    }

    // Did we see y?

    if (schedule_pos == old_schedule.size()) {
        old_schedule.swap(*schedule); // already swapped
        return false;
    }

    // Emit y

    schedule->push_back(old_schedule[schedule_pos]);
    ++schedule_pos;

    // Emit x and b''

    for (size_t i = 0; i < bprimeprime.size(); ++i) {
        schedule->push_back(bprimeprime[i]);
    }

    // Emit c

    for (; schedule_pos < old_schedule.size(); ++schedule_pos) {
        schedule->push_back(old_schedule[schedule_pos]);
    }

    return true;
}

void explore(const char* initial_schedule, const char* initial_base_dir) {

    /*
     * Re-implementation of the WAVE[1] algorithm.
     *
     * This is only an approximation of the PF heuristic of the WAVE algorithm.
     * The WAVE algorithm states that we:
     * 1. Must generate all possible event sequences which are valid according to the HB relation and UI-HB relation
     * 2. Iteratively pick the event sequence generating highest coverage, measured in executed orderings of event pairs
     *
     * Unfortunately, generating all possible event sequences (within the given constraints) in step 1 is not feasible for
     * long event sequences (2000+). Thus, we must approximate the algorithm.
     *
     * This implementation:
     * 1. Generates X number of event sequences (copies of the original sequence).
     * 2. Generates a list of event pair orderings and randomizes the list
     * 3. Sequentially step through the list, applying each event ordering (if possible) to the current event sequence (of the X sequences).
     *    - For each reversals the pointer to the current event sequence is moved to the next event sequence. If all event sequences have been
     *      used then reset to the first sequence.
     *
     * In summary, we try to apply all reversals, applying the reversals at random to the event sequences.
     *
     * Complexity: O(n^3). Worst case (with no HB relations), we need to handle n^2 ordered event pairs. For each reversal, we are will do at most
     * n operations (generating the new event sequence).
     *
     */

    std::tr1::shared_ptr<TraceReorder> init_reorder = std::tr1::shared_ptr<TraceReorder>(new TraceReorder());
    init_reorder->LoadSchedule(initial_schedule);

    std::string executed_er_log = std::string(initial_base_dir) + "/" + "ER_actionlog";
    RaceApp new_race_app(0, executed_er_log.c_str(), false);
    const VarsInfo& vinfo = new_race_app.vinfo();
    const EventGraphInterface* hb = vinfo.fast_event_graph();

    Schedule schedule = init_reorder->RemoveSpecialMarkers(init_reorder->GetSchedule());

    printf("Schedule size: %d\n", schedule.size());

    //size_t numEvents = vinfo.numNodes()+1;
    //std::vector<bool> pending_reversals(numEvents*numEvents, false);

    std::vector<std::pair<StrictEventID, StrictEventID> > reversal_queue;

    size_t numReversals = 0; // we want to reverse
    size_t numReversed = 0; // we did reverse

    for (size_t i = 0; i < schedule.size(); ++i) {
        for (size_t j = i+1; j < schedule.size(); ++j) {

            StrictEventID event1 = schedule[i];
            StrictEventID event2 = schedule[j];

            if (!hb->areOrdered(event1, event2)) {
                //pending_reversals[event2*numEvents + event1] = true;

                reversal_queue.push_back(std::pair<StrictEventID, StrictEventID>(event1, event2));
                ++numReversals;
            }
        }
    }

    std::random_shuffle(reversal_queue.begin(), reversal_queue.end());

    printf("Possible reversals: %d\n", numReversals);

    size_t schedules_pointer = 0;
    std::vector<Schedule> schedules(FLAGS_iteration_bound);

    for (size_t i = 0; i < FLAGS_iteration_bound; ++i) {
        schedules[i] = schedule; // copy schedule to vector
    }

    while (!reversal_queue.empty()) {
        std::pair<StrictEventID, StrictEventID>& reversal = reversal_queue.back();

        Schedule& next_schedule = schedules[schedules_pointer];
        bool ok = reverse_schedule(hb, &next_schedule, reversal.first, reversal.second);

        if (ok) {
            ++numReversed;
        }

        schedules_pointer = (schedules_pointer + 1) % schedules.size();
        reversal_queue.pop_back();
    }

    printf("Reversed: %d\n", numReversed);
    printf("Running %d schedules\n", FLAGS_iteration_bound);

    int all_schedules = 0;
    int successful_reverses = 0;
    int successful_schedules = 0;

    for (size_t i = 0; i < schedules.size(); ++i) {

        std::stringstream name;
        name << "iteration" << i;

        std::vector<int> next_schedule;
        next_schedule.push_back(-1);
        next_schedule.push_back(-1);

        for (size_t j = 0; j < schedules[i].size(); ++j) {
            next_schedule.push_back(schedules[i][j]);
        }

        init_reorder->SaveSchedule("new_wave_schedule.data", next_schedule);

        std::string executed_race_dir;
        std::string executed_schedule_log;
        std::string executed_er_log;

        ++all_schedules;
        ++successful_reverses;

        bool ok = performSchedule(name.str(), "base", initial_base_dir, "new_wave_schedule.data",
                                 &executed_race_dir, &executed_schedule_log, &executed_er_log);

        if (ok) {
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

    if (FLAGS_in_schedule_file.empty()) {
        fprintf(stderr, "  --in_schedule_file is a mandatory parameter.\n");
        return -1;
    }

    if (FLAGS_in_dir.empty()) {
        fprintf(stderr, "  --in_dir is a mandatory parameter.\n");
        return -1;
    }

    explore(FLAGS_in_schedule_file.c_str(), FLAGS_in_dir.c_str());

	return 0;
}
