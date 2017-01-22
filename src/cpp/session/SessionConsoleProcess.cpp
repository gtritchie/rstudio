/*
 * SessionConsoleProcess.cpp
 *
 * Copyright (C) 2009-17 by RStudio, Inc.
 *
 * Unless you have received this program directly from RStudio pursuant
 * to the terms of a commercial license agreement with RStudio, then
 * this program is licensed to you under the terms of version 3 of the
 * GNU Affero General Public License. This program is distributed WITHOUT
 * ANY EXPRESS OR IMPLIED WARRANTY, INCLUDING THOSE OF NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Please refer to the
 * AGPL (http://www.gnu.org/licenses/agpl-3.0.txt) for more details.
 *
 */

#include <session/SessionConsoleProcess.hpp>

#include <boost/regex.hpp>
#include <boost/foreach.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include <core/json/JsonRpc.hpp>
#include <core/system/Process.hpp>
#include <core/system/ShellUtils.hpp>
#include <core/Exec.hpp>
#include <core/SafeConvert.hpp>
#include <core/Settings.hpp>

#include <core/system/Environment.hpp>

#include <session/SessionModuleContext.hpp>

#include "session-config.h"

#ifdef RSTUDIO_SERVER
#include <core/system/Crypto.hpp>
#endif

#define kConsoleIndex "INDEX"

using namespace rstudio::core;

namespace rstudio {
namespace session {
namespace console_process {

namespace {
   typedef std::map<std::string, boost::shared_ptr<ConsoleProcess> > ProcTable;
   ProcTable s_procs;
   FilePath s_consoleProcPath;
   FilePath s_consoleProcIndexPath;
} // anonymous namespace

void saveConsoleProcesses(bool terminatedNormally = true);

ConsoleProcess::ConsoleProcess(boost::shared_ptr<ConsoleProcessInfo> procInfo)
   : procInfo_(procInfo), interrupt_(false), newCols_(-1), newRows_(-1),
     childProcsSent_(false)
{
   regexInit();

   // When we retrieve from outputBuffer, we only want complete lines. Add a
   // dummy \n so we can tell the first line is a complete line.
   procInfo_->appendToOutputBuffer('\n');
}
   
ConsoleProcess::ConsoleProcess(const std::string& command,
                               const core::system::ProcessOptions& options,
                               boost::shared_ptr<ConsoleProcessInfo> procInfo)
   : command_(command), options_(options), procInfo_(procInfo),
     interrupt_(false), newCols_(-1), newRows_(-1), childProcsSent_(false)
{
   commonInit();
}
   
ConsoleProcess::ConsoleProcess(const std::string& program,
                               const std::vector<std::string>& args,
                               const core::system::ProcessOptions& options,
                               boost::shared_ptr<ConsoleProcessInfo> procInfo)
   : program_(program), args_(args), options_(options), procInfo_(procInfo),
     interrupt_(false), newCols_(-1), newRows_(-1), childProcsSent_(false)
{
   commonInit();
}

void ConsoleProcess::regexInit()
{
   controlCharsPattern_ = boost::regex("[\\r\\b]");
   promptPattern_ = boost::regex("^(.+)[\\W_]( +)$");
}

void ConsoleProcess::commonInit()
{
   regexInit();
   procInfo_->ensureHandle();

   // always redirect stderr to stdout so output is interleaved
   options_.redirectStdErrToStdOut = true;

   if (interactionMode() != InteractionNever)
   {
#ifdef _WIN32
      // NOTE: We use consoleio.exe here in order to make sure svn.exe password
      // prompting works properly
      options_.createNewConsole = true;

      FilePath consoleIoPath = session::options().consoleIoPath();

      // if this is as runProgram then fixup the program and args
      if (!program_.empty())
      {
         // build new args
         shell_utils::ShellArgs args;
         args << program_;
         args << args_;

         // fixup program_ and args_ so we run the consoleio.exe proxy
         program_ = consoleIoPath.absolutePathNative();
         args_ = args;
      }
      // if this is a runCommand then prepend consoleio.exe to the command
      else if (!command_.empty())
      {
         command_ = shell_utils::escape(consoleIoPath) + " " + command_;
      }
      else // terminal
      {
         options_.consoleIoPath = shell_utils::escape(consoleIoPath);
      }
#else
      // request a pseudoterminal if this is an interactive console process
      options_.pseudoterminal = core::system::Pseudoterminal(options_.cols,
                                                             options_.rows);

      // define TERM (but first make sure we have an environment
      // block to modify)
      if (!options_.environment)
      {
         core::system::Options childEnv;
         core::system::environment(&childEnv);
         options_.environment = childEnv;
      }
      
      core::system::setenv(&(options_.environment.get()), "TERM",
                           options_.smartTerminal ? core::system::kSmartTerm :
                                                    core::system::kDumbTerm);
#endif
   }


   // When we retrieve from outputBuffer, we only want complete lines. Add a
   // dummy \n so we can tell the first line is a complete line.
   procInfo_->appendToOutputBuffer('\n');
}

std::string ConsoleProcess::bufferedOutput() const
{
   if (options_.smartTerminal)
      return "";

   return procInfo_->bufferedOutput();
}

void ConsoleProcess::setPromptHandler(
      const boost::function<bool(const std::string&, Input*)>& onPrompt)
{
   onPrompt_ = onPrompt;
}

Error ConsoleProcess::start()
{
   if (procInfo_->isStarted())
      return Success();

   Error error;
   if (!command_.empty())
   {
      error = module_context::processSupervisor().runCommand(
                                 command_, options_, createProcessCallbacks());
   }
   else if (!program_.empty())
   {
      error = module_context::processSupervisor().runProgram(
                          program_, args_, options_, createProcessCallbacks());
   }
   else
   {
      error = module_context::processSupervisor().runTerminal(
                          options_, createProcessCallbacks());
   }
   if (!error)
      procInfo_->setIsStarted(true);
   return error;
}

void ConsoleProcess::enqueInput(const Input& input)
{
   inputQueue_.push(input);
}

void ConsoleProcess::interrupt()
{
   interrupt_ = true;
}

void ConsoleProcess::resize(int cols, int rows)
{
   newCols_ = cols;
   newRows_ = rows;
}

bool ConsoleProcess::onContinue(core::system::ProcessOperations& ops)
{
   // full stop interrupt if requested
   if (interrupt_)
      return false;

   // process input queue
   while (!inputQueue_.empty())
   {
      // pop input
      Input input = inputQueue_.front();
      inputQueue_.pop();

      // pty interrupt
      if (input.interrupt)
      {
         Error error = ops.ptyInterrupt();
         if (error)
            LOG_ERROR(error);

         if (input.echoInput)
            appendToOutputBuffer("^C");
      }

      // text input
      else
      {
         std::string inputText = input.text;
#ifdef _WIN32
         string_utils::convertLineEndings(&inputText, string_utils::LineEndingWindows);
#endif
         Error error = ops.writeToStdin(inputText, false);
         if (error)
            LOG_ERROR(error);

         if (!options_.smartTerminal) // smart terminal does echo via pty
         {
            if (input.echoInput)
               appendToOutputBuffer(inputText);
            else
               appendToOutputBuffer("\n");
         }
      }
   }

   if (newCols_ != -1 && newRows_ != -1)
   {
      ops.ptySetSize(newCols_, newRows_);
      newCols_ = -1;
      newRows_ = -1;
   }
   
   // continue
   return true;
}

Error ConsoleProcess::getLogFilePath(FilePath* pFile) const
{
   Error error = s_consoleProcPath.ensureDirectory();
   if (error)
   {
      return error;
   }

   *pFile = s_consoleProcPath.complete(procInfo_->getHandle());
   return Success();
}

void ConsoleProcess::deleteLogFile() const
{
   FilePath log;
   Error error = getLogFilePath(&log);
   if (error)
   {
      LOG_ERROR(error);
      return;
   }
   error = log.removeIfExists();
   if (error)
   {
      LOG_ERROR(error);
      return;
   }
}

std::string ConsoleProcess::getSavedBuffer() const
{
   std::string content;
   FilePath log;
   Error error = getLogFilePath(&log);
   if (error)
   {
      LOG_ERROR(error);
      return content;
   }

   if (!log.exists())
   {
      return "";
   }

   error = core::readStringFromFile(log, &content);
   if (error)
   {
      LOG_ERROR(error);
      return content;
   }
   return content;
}

void ConsoleProcess::appendToOutputBuffer(const std::string &str)
{
   // For modal console procs, store terminal output directly in the
   // ConsoleProcInfo INDEX
   if (procInfo_->getTerminalSequence() == kNoTerminal)
   {
      procInfo_->appendToOutputBuffer(str);
      return;
   }

   // For terminal tabs, store in a separate file.
   FilePath log;
   Error error = getLogFilePath(&log);
   if (error)
   {
      LOG_ERROR(error);
      return;
   }

   error = rstudio::core::appendToFile(log, str);
   if (error)
   {
      LOG_ERROR(error);
   }
}

void ConsoleProcess::enqueOutputEvent(const std::string &output, bool error)
{
   // copy to output buffer
   appendToOutputBuffer(output);

   // If there's more output than the client can even show, then
   // truncate it to the amount that the client can show. Too much
   // output can overwhelm the client, making it unresponsive.
   std::string trimmedOutput = output;
   string_utils::trimLeadingLines(procInfo_->getMaxOutputLines(), &trimmedOutput);

   json::Object data;
   data["handle"] = handle();
   data["error"] = error;
   data["output"] = trimmedOutput;
   module_context::enqueClientEvent(
         ClientEvent(client_events::kConsoleProcessOutput, data));
}

void ConsoleProcess::onStdout(core::system::ProcessOperations& ops,
                              const std::string& output)
{
   if (options_.smartTerminal)
   {
      enqueOutputEvent(output, false);
      return;
   }
   
   // convert line endings to posix
   std::string posixOutput = output;
   string_utils::convertLineEndings(&posixOutput,
                                    string_utils::LineEndingPosix);

   // process as normal output or detect a prompt if there is one
   if (boost::algorithm::ends_with(posixOutput, "\n"))
   {
      enqueOutputEvent(posixOutput, false);
   }
   else
   {
      // look for the last newline and take the content after
      // that as the prompt
      std::size_t lastLoc = posixOutput.find_last_of("\n\f");
      if (lastLoc != std::string::npos)
      {
         enqueOutputEvent(posixOutput.substr(0, lastLoc), false);
         maybeConsolePrompt(ops, posixOutput.substr(lastLoc + 1));
      }
      else
      {
         maybeConsolePrompt(ops, posixOutput);
      }
   }
}

void ConsoleProcess::maybeConsolePrompt(core::system::ProcessOperations& ops,
                                        const std::string& output)
{
   boost::smatch smatch;

   // treat special control characters as output rather than a prompt
   if (boost::regex_search(output, smatch, controlCharsPattern_))
      enqueOutputEvent(output, false);

   // make sure the output matches our prompt pattern
   if (!boost::regex_match(output, smatch, promptPattern_))
      enqueOutputEvent(output, false);

   // it is a prompt
   else
      handleConsolePrompt(ops, output);
}

void ConsoleProcess::handleConsolePrompt(core::system::ProcessOperations& ops,
                                         const std::string& prompt)
{
   // if there is a custom prmopt handler then give it a chance to
   // handle the prompt first
   if (onPrompt_)
   {
      Input input;
      if (onPrompt_(prompt, &input))
      {
         if (!input.empty())
         {
            enqueInput(input);
         }
         else
         {
            Error error = ops.terminate();
            if (error)
              LOG_ERROR(error);
         }

         return;
      }

   }

   // enque a prompt event
   json::Object data;
   data["handle"] = handle();
   data["prompt"] = prompt;
   module_context::enqueClientEvent(
         ClientEvent(client_events::kConsoleProcessPrompt, data));
}

void ConsoleProcess::onSuspend()
{
   procInfo_->onSuspend();
}

void ConsoleProcess::onExit(int exitCode)
{
   procInfo_->setExitCode(exitCode);

   json::Object data;
   data["handle"] = handle();
   data["exitCode"] = exitCode;
   module_context::enqueClientEvent(
         ClientEvent(client_events::kConsoleProcessExit, data));

   onExit_(exitCode);
}

void ConsoleProcess::onHasSubprocs(bool hasSubprocs)
{
   if (hasSubprocs != procInfo_->getHasChildProcs() || !childProcsSent_)
   {
      procInfo_->setHasChildProcs(hasSubprocs);

      json::Object subProcs;
      subProcs["handle"] = handle();
      subProcs["subprocs"] = procInfo_->getHasChildProcs();
      module_context::enqueClientEvent(
            ClientEvent(client_events::kTerminalSubprocs, subProcs));
      childProcsSent_ = true;
   }
}

core::json::Object ConsoleProcess::toJson() const
{
   return procInfo_->toJson();
}

boost::shared_ptr<ConsoleProcess> ConsoleProcess::fromJson(
                                             core::json::Object &obj)
{
   boost::shared_ptr<ConsoleProcessInfo> pProcInfo(ConsoleProcessInfo::fromJson(obj));
   boost::shared_ptr<ConsoleProcess> pProc(new ConsoleProcess(pProcInfo));
   return pProc;
}

core::system::ProcessCallbacks ConsoleProcess::createProcessCallbacks()
{
   core::system::ProcessCallbacks cb;
   cb.onContinue = boost::bind(&ConsoleProcess::onContinue, ConsoleProcess::shared_from_this(), _1);
   cb.onStdout = boost::bind(&ConsoleProcess::onStdout, ConsoleProcess::shared_from_this(), _1, _2);
   cb.onExit = boost::bind(&ConsoleProcess::onExit, ConsoleProcess::shared_from_this(), _1);
   if (options_.reportHasSubprocs)
   {
      cb.onHasSubprocs = boost::bind(&ConsoleProcess::onHasSubprocs, ConsoleProcess::shared_from_this(), _1);
   }
   return cb;
}

Error procStart(const json::JsonRpcRequest& request,
                json::JsonRpcResponse* pResponse)
{
   std::string handle;
   Error error = json::readParams(request.params, &handle);
   if (error)
      return error;
   ProcTable::const_iterator pos = s_procs.find(handle);
   if (pos != s_procs.end())
   {
      return pos->second->start();
   }
   else
   {
      return systemError(boost::system::errc::invalid_argument,
                         ERROR_LOCATION);
   }
}

Error procInterrupt(const json::JsonRpcRequest& request,
                    json::JsonRpcResponse* pResponse)
{
   std::string handle;
   Error error = json::readParams(request.params, &handle);
   if (error)
      return error;
   ProcTable::const_iterator pos = s_procs.find(handle);
   if (pos != s_procs.end())
   {
      pos->second->interrupt();
      return Success();
   }
   else
   {
      return systemError(boost::system::errc::invalid_argument,
                         ERROR_LOCATION);
   }
}

Error procReap(const json::JsonRpcRequest& request,
               json::JsonRpcResponse* pResponse)
{
   std::string handle;
   Error error = json::readParams(request.params, &handle);
   if (error)
      return error;

   ProcTable::const_iterator pos = s_procs.find(handle);
   if (pos != s_procs.end())
   {
      pos->second->deleteLogFile();
      if (s_procs.erase(handle))
      {
         saveConsoleProcesses();
         return Success();
      }
   }
   
   return systemError(boost::system::errc::invalid_argument,
                      ERROR_LOCATION);
}

Error procWriteStdin(const json::JsonRpcRequest& request,
                     json::JsonRpcResponse* pResponse)
{
   std::string handle;
   Error error = json::readParam(request.params, 0, &handle);
   if (error)
      return error;

   ConsoleProcess::Input input;
   error = json::readObjectParam(request.params, 1,
                                 "interrupt", &input.interrupt,
                                 "text", &input.text,
                                 "echo_input", &input.echoInput);
   if (error)
      return error;

   ProcTable::const_iterator pos = s_procs.find(handle);
   if (pos != s_procs.end())
   {
#ifdef RSTUDIO_SERVER
      if (session::options().programMode() == kSessionProgramModeServer)
      {
         if (!input.interrupt)
         {
            error = core::system::crypto::rsaPrivateDecrypt(input.text,
                                                            &input.text);
            if (error)
               return error;
         }
      }
#endif

      pos->second->enqueInput(input);

      return Success();
   }
   else
   {
      return systemError(boost::system::errc::invalid_argument,
                         ERROR_LOCATION);
   }
}

Error procSetSize(const json::JsonRpcRequest& request,
                        json::JsonRpcResponse* pResponse)
{
   std::string handle;
   int cols, rows;
   Error error = json::readParams(request.params,
                                  &handle,
                                  &cols,
                                  &rows);
   if (error)
      return error;
   
   ProcTable::const_iterator pos = s_procs.find(handle);
   if (pos != s_procs.end())
   {
      pos->second->resize(cols, rows);
      return Success();

   }
   else
   {
      return systemError(boost::system::errc::invalid_argument,
                         ERROR_LOCATION);
   }
}

Error procSetCaption(const json::JsonRpcRequest& request,
                           json::JsonRpcResponse* pResponse)
{
   std::string handle;
   std::string caption;
   
   Error error = json::readParams(request.params,
                                  &handle,
                                  &caption);
   if (error)
      return error;
   
   ProcTable::const_iterator pos = s_procs.find(handle);
   if (pos == s_procs.end())
   {
      return systemError(boost::system::errc::invalid_argument, ERROR_LOCATION);
   }
   
   pos->second->setCaption(caption);
   saveConsoleProcesses();
   return Success();
}

Error procSetTitle(const json::JsonRpcRequest& request,
                         json::JsonRpcResponse* pResponse)
{
   std::string handle;
   std::string title;
   
   Error error = json::readParams(request.params,
                                  &handle,
                                  &title);
   if (error)
      return error;
   
   ProcTable::const_iterator pos = s_procs.find(handle);
   if (pos == s_procs.end())
   {
      return systemError(boost::system::errc::invalid_argument, ERROR_LOCATION);
   }
   
   pos->second->setTitle(title);
   return Success();
}

Error procEraseBuffer(const json::JsonRpcRequest& request,
                      json::JsonRpcResponse* pResponse)
{
   std::string handle;

   Error error = json::readParams(request.params,
                                  &handle);
   if (error)
      return error;

   ProcTable::const_iterator pos = s_procs.find(handle);
   if (pos == s_procs.end())
   {
      return systemError(boost::system::errc::invalid_argument, ERROR_LOCATION);
   }

   pos->second->deleteLogFile();
   return Success();
}

Error procGetBuffer(const json::JsonRpcRequest& request,
                    json::JsonRpcResponse* pResponse)
{
   std::string handle;

   Error error = json::readParams(request.params,
                                  &handle);
   if (error)
      return error;

   ProcTable::const_iterator pos = s_procs.find(handle);
   if (pos == s_procs.end())
   {
      return systemError(boost::system::errc::invalid_argument, ERROR_LOCATION);
   }

   // TODO (gary) throttle (or chunk) output to avoid overwhelming the
   // client; e.g. we might return a flag/handle to allow client to know to make
   // more of these calls until buffer has been completely fetched
   pResponse->setResult(pos->second->getSavedBuffer());

   return Success();
}

boost::shared_ptr<ConsoleProcess> ConsoleProcess::create(
      const std::string& command,
      core::system::ProcessOptions options,
      boost::shared_ptr<ConsoleProcessInfo> procInfo)
{
   options.terminateChildren = true;
   boost::shared_ptr<ConsoleProcess> ptrProc(
         new ConsoleProcess(command, options, procInfo));
   s_procs[ptrProc->handle()] = ptrProc;
   saveConsoleProcesses();
   return ptrProc;
}

boost::shared_ptr<ConsoleProcess> ConsoleProcess::create(
      const std::string& program,
      const std::vector<std::string>& args,
      core::system::ProcessOptions options,
      boost::shared_ptr<ConsoleProcessInfo> procInfo)
{
   options.terminateChildren = true;
   boost::shared_ptr<ConsoleProcess> ptrProc(
         new ConsoleProcess(program, args, options, procInfo));
   s_procs[ptrProc->handle()] = ptrProc;
   saveConsoleProcesses();
   return ptrProc;
}

// supports reattaching to a running process, or creating a new process with
// previously used handle
boost::shared_ptr<ConsoleProcess> ConsoleProcess::createTerminalProcess(
      core::system::ProcessOptions options,
      boost::shared_ptr<ConsoleProcessInfo> procInfo)
{
   std::string command;
   if (procInfo->getAllowRestart() && !procInfo->getHandle().empty())
   {
      // return existing ConsoleProcess if it is still running
      ProcTable::const_iterator pos = s_procs.find(procInfo->getHandle());
      if (pos != s_procs.end() && pos->second->isStarted())
      {
         // Jiggle the size of the pseudo-terminal, this will force the app
         // to refresh itself; this does rely on the host performing a second
         // resize to the actual available size. Clumsy, but so far this is
         // the best I've come up with.
         pos->second->resize(25, 5);
         return pos->second;
      }
      
      // Create new process with previously used handle
      options.terminateChildren = true;
      boost::shared_ptr<ConsoleProcess> ptrProc(
            new ConsoleProcess(command, options, procInfo));
      s_procs[ptrProc->handle()] = ptrProc;
      saveConsoleProcesses();
      return ptrProc;
   }
   
   // otherwise create a new one
   return create(command, options, procInfo);
}

void PasswordManager::attach(
                  boost::shared_ptr<console_process::ConsoleProcess> pCP,
                  bool showRememberOption)
{
   pCP->setPromptHandler(boost::bind(&PasswordManager::handlePrompt,
                                       this,
                                       pCP->handle(),
                                       _1,
                                       showRememberOption,
                                       _2));

   pCP->onExit().connect(boost::bind(&PasswordManager::onExit,
                                       this,
                                       pCP->handle(),
                                       _1));
}

bool PasswordManager::handlePrompt(const std::string& cpHandle,
                                   const std::string& prompt,
                                   bool showRememberOption,
                                   ConsoleProcess::Input* pInput)
{
   // is this a password prompt?
   boost::smatch match;
   if (boost::regex_match(prompt, match, promptPattern_))
   {
      // see if it matches any of our existing cached passwords
      std::vector<CachedPassword>::const_iterator it =
                  std::find_if(passwords_.begin(),
                               passwords_.end(),
                               boost::bind(&hasPrompt, _1, prompt));
      if (it != passwords_.end())
      {
         // cached password
         *pInput = ConsoleProcess::Input(it->password + "\n", false);
      }
      else
      {
         // prompt for password
         std::string password;
         bool remember;
         if (promptHandler_(prompt, showRememberOption, &password, &remember))
         {

            // cache the password (but also set the remember flag so it
            // will be removed from the cache when the console process
            // exits if the user chose not to remember).
            CachedPassword cachedPassword;
            cachedPassword.cpHandle = cpHandle;
            cachedPassword.prompt = prompt;
            cachedPassword.password = password;
            cachedPassword.remember = remember;
            passwords_.push_back(cachedPassword);

            // interactively entered password
            *pInput = ConsoleProcess::Input(password + "\n", false);
         }
         else
         {
            // user cancelled
            *pInput = ConsoleProcess::Input();
         }
      }

      return true;
   }
   // not a password prompt so ignore
   else
   {
      return false;
   }
}

void PasswordManager::onExit(const std::string& cpHandle,
                             int exitCode)
{
   // if a process exits with an error then remove any cached
   // passwords which originated from that process
   if (exitCode != EXIT_SUCCESS)
   {
      passwords_.erase(std::remove_if(passwords_.begin(),
                                      passwords_.end(),
                                      boost::bind(&hasHandle, _1, cpHandle)),
                       passwords_.end());
   }

   // otherwise remove any cached password for this process which doesn't
   // have its remember flag set
   else
   {
      passwords_.erase(std::remove_if(passwords_.begin(),
                                      passwords_.end(),
                                      boost::bind(&forgetOnExit, _1, cpHandle)),
                       passwords_.end());
   }
}


bool PasswordManager::hasPrompt(const CachedPassword& cachedPassword,
                                const std::string& prompt)
{
   return cachedPassword.prompt == prompt;
}

bool PasswordManager::hasHandle(const CachedPassword& cachedPassword,
                                const std::string& cpHandle)
{
   return cachedPassword.cpHandle == cpHandle;
}

bool PasswordManager::forgetOnExit(const CachedPassword& cachedPassword,
                                   const std::string& cpHandle)
{
   return hasHandle(cachedPassword, cpHandle) && !cachedPassword.remember;
}

core::json::Array processesAsJson()
{
   json::Array procInfos;
   for (ProcTable::const_iterator it = s_procs.begin();
        it != s_procs.end();
        it++)
   {
      procInfos.push_back(it->second->toJson());
   }
   return procInfos;
}

std::string serializeConsoleProcs(bool suspend = false)
{
   json::Array array;
   for (ProcTable::const_iterator it = s_procs.begin();
        it != s_procs.end();
        it++)
   {
      if (suspend)
      {
         it->second->onSuspend();
      }
      array.push_back(it->second->toJson());
   }

   std::ostringstream ostr;
   json::write(array, ostr);
   return ostr.str();
}

void deserializeConsoleProcs(const std::string& jsonStr)
{
   if (jsonStr.empty())
      return;
   json::Value value;
   if (!json::parse(jsonStr, &value))
   {
      LOG_WARNING_MESSAGE("invalid console process json: " + jsonStr);
      return;
   }

   json::Array procs = value.get_array();
   for (json::Array::iterator it = procs.begin();
        it != procs.end();
        it++)
   {
      boost::shared_ptr<ConsoleProcess> proc =
                                    ConsoleProcess::fromJson(it->get_obj());
      s_procs[proc->handle()] = proc;
   }
}

void loadConsoleProcesses()
{
   if (!s_consoleProcIndexPath.exists())
      return;

   std::string contents;
   Error error = rstudio::core::readStringFromFile(s_consoleProcIndexPath, &contents);
   if (error)
   {
      LOG_ERROR(error);
      return;
   }

   deserializeConsoleProcs(contents);

   // Delete orphaned buffer files
   std::vector<FilePath> children;
   error = s_consoleProcPath.children(&children);
   if (error)
   {
      LOG_ERROR(error);
      return;
   }
   BOOST_FOREACH(const FilePath& child, children)
   {
      // Don't erase the INDEX or any subfolders
      if (!child.filename().compare(kConsoleIndex) || child.isDirectory())
         continue;

      ProcTable::const_iterator pos = s_procs.find(child.filename());
      if (pos == s_procs.end())
      {
         error = child.remove();
         if (error)
            LOG_ERROR(error);
      }
   }
}

void saveConsoleProcesses(bool terminatedNormally)
{
   if (!terminatedNormally)
      return;
   Error error = rstudio::core::writeStringToFile(s_consoleProcIndexPath,
                                                  serializeConsoleProcs());
   if (error)
      LOG_ERROR(error);
}

void onSuspend(core::Settings* pSettings)
{
   serializeConsoleProcs(true /*suspend*/);
}

void onResume(const core::Settings& /*settings*/)
{
}

Error initialize()
{
   using boost::bind;
   using namespace module_context;

   // storage for session-scoped console/terminal metadata
   s_consoleProcPath = module_context::scopedScratchPath().complete("console");
   Error error = s_consoleProcPath.ensureDirectory();
   if (error)
      return error;
   s_consoleProcIndexPath = s_consoleProcPath.complete(kConsoleIndex);

   events().onShutdown.connect(saveConsoleProcesses);
   addSuspendHandler(SuspendHandler(boost::bind(onSuspend, _2), onResume));

   loadConsoleProcesses();

   // install rpc methods
   ExecBlock initBlock ;
   initBlock.addFunctions()
      (bind(registerRpcMethod, "process_start", procStart))
      (bind(registerRpcMethod, "process_interrupt", procInterrupt))
      (bind(registerRpcMethod, "process_reap", procReap))
      (bind(registerRpcMethod, "process_write_stdin", procWriteStdin))
      (bind(registerRpcMethod, "process_set_size", procSetSize))
      (bind(registerRpcMethod, "process_set_caption", procSetCaption))
      (bind(registerRpcMethod, "process_set_title", procSetTitle))
      (bind(registerRpcMethod, "process_erase_buffer", procEraseBuffer))
      (bind(registerRpcMethod, "process_get_buffer", procGetBuffer));

   return initBlock.execute();
}

} // namespace console_process
} // namespace session
} // namespace rstudio
