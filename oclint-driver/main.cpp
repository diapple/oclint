#include <dlfcn.h>
#include <dirent.h>
#include <unistd.h>
#include <iostream>
#include <fstream>
#include <ctime>
#include <string>

#include <llvm/ADT/SmallString.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/FileSystem.h>
#include <clang/Driver/OptTable.h>
#include <clang/Driver/Options.h>
#include <clang/Tooling/CommonOptionsParser.h>

#include "oclint/Analyzer.h"
#include "oclint/CompilerInstance.h"
#include "oclint/Driver.h"
#include "oclint/GenericException.h"
#include "oclint/Reporter.h"
#include "oclint/Results.h"
#include "oclint/RuleBase.h"
#include "oclint/RuleConfiguration.h"
#include "oclint/RuleSet.h"
#include "oclint/RulesetBasedAnalyzer.h"
#include "oclint/ViolationSet.h"
#include "oclint/Violation.h"

using namespace std;
using namespace llvm;
using namespace llvm::sys;
using namespace clang;
using namespace clang::driver;
using namespace clang::tooling;

/* ----------------
   input and output
   ---------------- */

cl::opt<string> argOutput("o",
    cl::desc("Write output to <path>"),
    cl::value_desc("path"),
    cl::init("-"));

/* --------------------
   oclint configuration
   -------------------- */

cl::opt<string> argReportType("report-type",
    cl::desc("Change output report type"),
    cl::value_desc("name"),
    cl::init("text"));
cl::list<string> argRulesPath("R",
    cl::Prefix,
    cl::desc("Add directory to rule loading path"),
    cl::value_desc("directory"),
    cl::ZeroOrMore);
cl::list<string> argRuleConfiguration("rc",
    cl::desc("Override the default behavior of rules"),
    cl::value_desc("parameter>=<value"),
    cl::ZeroOrMore);
cl::opt<int> argMaxP1("max-priority-1",
    cl::desc("The max allowed number of priority 1 violations"),
    cl::value_desc("threshold"),
    cl::init(0));
cl::opt<int> argMaxP2("max-priority-2",
    cl::desc("The max allowed number of priority 2 violations"),
    cl::value_desc("threshold"),
    cl::init(10));
cl::opt<int> argMaxP3("max-priority-3",
    cl::desc("The max allowed number of priority 3 violations"),
    cl::value_desc("threshold"),
    cl::init(20));

/* -------------
   libTooling cl
   ------------- */

static cl::extrahelp CommonHelp(CommonOptionsParser::HelpMessage);
static cl::extrahelp MoreHelp(
    "For more information, please visit http://oclint.org\n"
);
static OwningPtr<OptTable> Options(createDriverOptTable());


static string absoluteWorkingPath("");
static oclint::Reporter *selectedReporter = NULL;

void preserveWorkingPath()
{
    char path[300];
    if (getcwd(path, 300))
    {
        absoluteWorkingPath = string(path);
    }
}

string getExecutablePath(const char *argv)
{
    llvm::SmallString<128> installedPath(argv);
    if (path::filename(installedPath) == installedPath)
    {
        Path intermediatePath = FindProgramByName(path::filename(installedPath.str()));
        if (!intermediatePath.empty())
        {
            installedPath = intermediatePath.str();
        }
    }
    fs::make_absolute(installedPath);
    installedPath = path::parent_path(installedPath);
    return string(installedPath.c_str());
}

void dynamicLoadRules(string ruleDirPath)
{
    DIR *pDir = opendir(ruleDirPath.c_str());
    if (pDir != NULL)
    {
        struct dirent *dirp;
        while ((dirp = readdir(pDir)))
        {
            if (dirp->d_name[0] == '.')
            {
                continue;
            }
            string rulePath = ruleDirPath + "/" + string(dirp->d_name);
            if (dlopen(rulePath.c_str(), RTLD_LAZY) == NULL)
            {
                cerr << dlerror() << endl;
                closedir(pDir);
                throw oclint::GenericException("cannot open dynamic library: " + rulePath);
            }
        }
        closedir(pDir);
    }
}

void consumeArgRulesPath(const char* executablePath)
{
    if (argRulesPath.size() == 0)
    {
        string exeStrPath = getExecutablePath(executablePath);
        string defaultRulePath = exeStrPath + "/../lib/oclint/rules";
        dynamicLoadRules(defaultRulePath);
    }
    else
    {
        for (unsigned i = 0; i < argRulesPath.size(); ++i)
        {
            dynamicLoadRules(argRulesPath[i]);
        }
    }
}

void consumeRuleConfigurations()
{
    for (unsigned i = 0; i < argRuleConfiguration.size(); ++i)
    {
        string configuration = argRuleConfiguration[i];
        int indexOfSeparator = configuration.find_last_of("=");
        string key = configuration.substr(0, indexOfSeparator);
        string value = configuration.substr(indexOfSeparator + 1,
            configuration.size() - indexOfSeparator - 1);
        oclint::RuleConfiguration::addConfiguration(key, value);
    }
}

void loadReporter(const char* executablePath)
{
    selectedReporter = NULL;
    string exeStrPath = getExecutablePath(executablePath);
    string defaultReportersPath = exeStrPath + "/../lib/oclint/reporters";
    DIR *pDir = opendir(defaultReportersPath.c_str());
    if (pDir != NULL)
    {
        struct dirent *dirp;
        while ((dirp = readdir(pDir)))
        {
            if (dirp->d_name[0] == '.')
            {
                continue;
            }
            string reporterPath = defaultReportersPath + "/" + string(dirp->d_name);
            void *reporterHandle = dlopen(reporterPath.c_str(), RTLD_LAZY);
            if (reporterHandle == NULL)
            {
                cerr << dlerror() << endl;
                closedir(pDir);
                throw oclint::GenericException("cannot open dynamic library: " + reporterPath);
            }
            oclint::Reporter* (*createMethodPointer)();
            createMethodPointer = (oclint::Reporter* (*)())dlsym(reporterHandle, "create");
            oclint::Reporter* reporter = (oclint::Reporter*)createMethodPointer();
            if (reporter->name() == argReportType)
            {
                selectedReporter = reporter;
                break;
            }
        }
        closedir(pDir);
    }
    if (selectedReporter == NULL)
    {
        throw oclint::GenericException(
            "cannot find dynamic library for report type: " + argReportType);
    }
}

bool numberOfViolationsExceedThreshold(oclint::Results *results)
{
    return results->numberOfViolationsWithPriority(1) > argMaxP1 ||
        results->numberOfViolationsWithPriority(2) > argMaxP2 ||
        results->numberOfViolationsWithPriority(3) > argMaxP3;
}

ostream* outStream()
{
    if (argOutput == "-")
    {
        return &cout;
    }
    string absoluteOutputPath = argOutput.at(0) == '/' ?
        argOutput : absoluteWorkingPath + "/" + argOutput;
    ofstream *out = new ofstream(absoluteOutputPath.c_str());
    if (!out->is_open())
    {
        throw oclint::GenericException("cannot open report output file " + argOutput);
    }
    return out;
}

void disposeOutStream(ostream* out)
{
    if (out && argOutput != "-")
    {
        ofstream *fout = (ofstream *)out;
        fout->close();
    }
}

oclint::Reporter* reporter()
{
    return selectedReporter;
}

void printCompilerDiagnostics(ostream &out, oclint::ViolationSet &violationSet, string headerText)
{
    if (violationSet.numberOfViolations() > 0)
    {
        out << endl << headerText << endl << endl;
        for (int index = 0, numberOfViolations = violationSet.numberOfViolations();
            index < numberOfViolations; index++)
        {
            oclint::Violation violation = violationSet.getViolations().at(index);
            out << violation.path << ":" << violation.startLine << ":" << violation.startColumn;
            out << ": " << violation.message << endl;
        }
        out << endl;
    }
}

void printErrorLine(const char *errorMessage)
{
    cerr << endl << "oclint: error: " << errorMessage << endl;
}

enum ExitCode
{
    SUCCESS,
    RULE_NOT_FOUND,
    REPORTER_NOT_FOUND,
    ERROR_WHILE_PROCESSING,
    ERROR_WHILE_REPORTING,
    VIOLATIONS_EXCEED_THRESHOLD
};

int prepare(const char* executablePath)
{
    try
    {
        consumeArgRulesPath(executablePath);
    }
    catch (const exception& e)
    {
        printErrorLine(e.what());
        return RULE_NOT_FOUND;
    }
    if (oclint::RuleSet::numberOfRules() <= 0)
    {
        printErrorLine("no rule loaded");
        return RULE_NOT_FOUND;
    }
    try
    {
        loadReporter(executablePath);
    }
    catch (const exception& e)
    {
        printErrorLine(e.what());
        return REPORTER_NOT_FOUND;
    }
    consumeRuleConfigurations();
    preserveWorkingPath();

    return SUCCESS;
}

int main(int argc, const char **argv)
{
    CommonOptionsParser optionsParser(argc, argv);

    int prepareStatus = prepare(argv[0]);
    if (prepareStatus)
    {
        return prepareStatus;
    }

    oclint::ViolationSet errorSet;
    oclint::ViolationSet warningSet;
    oclint::RulesetBasedAnalyzer analyzer;
    oclint::Driver driver;
    try
    {
        driver.run(optionsParser.getCompilations(), optionsParser.getSourcePathList(),
            analyzer, errorSet, warningSet);
    }
    catch (const exception& e)
    {
        printErrorLine(e.what());
        return ERROR_WHILE_PROCESSING;
    }
    oclint::Results *results = oclint::Results::getInstance();

    try
    {
        ostream *out = outStream();

        // deal with compiler errors/warnings if any
        printCompilerDiagnostics(*out, errorSet, "Compiler Errors:\n(please aware that these errors"
            " will precent OCLint from analyzing those source code)");
        printCompilerDiagnostics(*out, warningSet, "Compiler Warnings:");

        reporter()->report(results, *out);
        disposeOutStream(out);
    }
    catch (const exception& e)
    {
        printErrorLine(e.what());
        return ERROR_WHILE_REPORTING;
    }

    if (numberOfViolationsExceedThreshold(results))
    {
        return VIOLATIONS_EXCEED_THRESHOLD;
    }
    return SUCCESS;
}
