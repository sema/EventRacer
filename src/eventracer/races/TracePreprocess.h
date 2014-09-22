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


#ifndef TRACEPREPROCESS_H_
#define TRACEPREPROCESS_H_

#include <string>

#include "ActionLog.h"
#include "StringSet.h"

class TracePreprocess {
public:
    TracePreprocess(ActionLog* log, const StringSet* vars, const StringSet* values)
        : m_log(log),
          m_vars(vars),
          m_values(values) {}

	virtual ~TracePreprocess() {}

	// Remove patterns:

    // Ignores specific locations declared through the commandline.
    void IgnoreLocation(const std::string& location);

    // write x [read|write x...]
    // x is always written to by an operation before being read
    void RemoveGlobalLocals();

    //   read x value A write x value A+
    // since they are likely incrementors of the form x++ which commute
    // this is safe to remove if a value is ONLY used for incrementation and never branched on
    void RemovePureIncrementation();

	//   read x value A write x value A
	// since they are likely lazy writes of the type:   x = x || expr.
	void RemoveEmptyReadWrites();

	// Remove writes that write the same value that was read before.
	void RemoveNopWrites();

	// Remove reads/writes that are in the same function that performed initialization of a memory location.
	// Initialization is first read followed by a write.
	void RemoveUpdatesInSameMethod();

private:
	void RemoveEmptyOperations();

	ActionLog* m_log;
    const StringSet* m_vars;
    const StringSet* m_values;
};

#endif /* TRACEPREPROCESS_H_ */
