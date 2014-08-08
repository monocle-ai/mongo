/**
*    Copyright (C) 2008 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "mongo/pch.h"

#include "mongo/db/initialize_server_global_state.h"

#include <boost/filesystem/operations.hpp>
#include <memory>

#ifndef _WIN32
#include <syslog.h>
#include <sys/types.h>
#include <sys/wait.h>
#endif

#include "mongo/base/init.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/security_key.h"
#include "mongo/db/cmdline.h"
#include "mongo/logger/logger.h"
#include "mongo/logger/message_event.h"
#include "mongo/logger/message_event_utf8_encoder.h"
#include "mongo/logger/rotatable_file_appender.h"
#include "mongo/logger/rotatable_file_manager.h"
#include "mongo/logger/rotatable_file_writer.h"
#include "mongo/logger/syslog_appender.h"
#include "mongo/platform/process_id.h"
#include "mongo/util/log.h"
#include "mongo/util/net/listen.h"
#include "mongo/util/processinfo.h"

namespace fs = boost::filesystem;

namespace mongo {

#ifndef _WIN32
    // support for exit value propagation with fork
    void launchSignal( int sig ) {
        if ( sig == SIGUSR2 ) {
            ProcessId cur = ProcessId::getCurrent();
            
            if ( cur == cmdLine.parentProc || cur == cmdLine.leaderProc ) {
                // signal indicates successful start allowing us to exit
                _exit(0);
            } 
        }
    }

    static void setupLaunchSignals() {
        verify( signal(SIGUSR2 , launchSignal ) != SIG_ERR );
    }

    void CmdLine::launchOk() {
        if ( cmdLine.doFork ) {
            // killing leader will propagate to parent
            verify( kill( cmdLine.leaderProc.toNative(), SIGUSR2 ) == 0 );
        }
    }
#endif


    static bool forkServer() {
#ifndef _WIN32
        if (cmdLine.doFork) {
            fassert(16447, !cmdLine.logpath.empty() || cmdLine.logWithSyslog);

            cout.flush();
            cerr.flush();

            cmdLine.parentProc = ProcessId::getCurrent();

            // facilitate clean exit when child starts successfully
            setupLaunchSignals();

            pid_t c = fork();
            if ( c ) {
                int pstat;
                waitpid(c, &pstat, 0);

                if ( WIFEXITED(pstat) ) {
                    if ( ! WEXITSTATUS(pstat) ) {
                        cout << "child process started successfully, parent exiting" << endl;
                    }

                    _exit( WEXITSTATUS(pstat) );
                }

                _exit(50);
            }

            if ( chdir("/") < 0 ) {
                cout << "Cant chdir() while forking server process: " << strerror(errno) << endl;
                ::_exit(-1);
            }
            setsid();

            cmdLine.leaderProc = ProcessId::getCurrent();

            pid_t c2 = fork();
            if ( c2 ) {
                int pstat;
                cout << "forked process: " << c2 << endl;
                waitpid(c2, &pstat, 0);

                if ( WIFEXITED(pstat) ) {
                    _exit( WEXITSTATUS(pstat) );
                }

                _exit(51);
            }

            // this is run in the final child process (the server)

            FILE* f = freopen("/dev/null", "w", stdout);
            if ( f == NULL ) {
                cout << "Cant reassign stdout while forking server process: " << strerror(errno) << endl;
                return false;
            }

            f = freopen("/dev/null", "w", stderr);
            if ( f == NULL ) {
                cout << "Cant reassign stderr while forking server process: " << strerror(errno) << endl;
                return false;
            }

            f = freopen("/dev/null", "r", stdin);
            if ( f == NULL ) {
                cout << "Cant reassign stdin while forking server process: " << strerror(errno) << endl;
                return false;
            }
        }
#endif  // !defined(_WIN32)
        return true;
    }

    void forkServerOrDie() {
        if (!forkServer())
            _exit(EXIT_FAILURE);
    }

    MONGO_INITIALIZER_GENERAL(ServerLogRedirection,
                              ("GlobalLogManager", "globalVariablesConfigured"),
                              ("default"))(
            InitializerContext*) {

        using logger::LogManager;
        using logger::MessageEventEphemeral;
        using logger::MessageEventDetailsEncoder;
        using logger::MessageEventWithContextEncoder;
        using logger::MessageLogDomain;
        using logger::RotatableFileAppender;
        using logger::StatusWithRotatableFileWriter;

#ifndef _WIN32
        using logger::SyslogAppender;

        if (cmdLine.logWithSyslog) {
            StringBuilder sb;
            sb << cmdLine.binaryName << "." << cmdLine.port;
            openlog(strdup(sb.str().c_str()), LOG_PID | LOG_CONS, LOG_USER);
            LogManager* manager = logger::globalLogManager();
            manager->getGlobalDomain()->clearAppenders();
            manager->getGlobalDomain()->attachAppender(
                    MessageLogDomain::AppenderAutoPtr(
                            new SyslogAppender<MessageEventEphemeral>(
                                    new logger::MessageEventWithContextEncoder)));
            manager->getNamedDomain("javascriptOutput")->attachAppender(
                    MessageLogDomain::AppenderAutoPtr(
                            new SyslogAppender<MessageEventEphemeral>(
                                    new logger::MessageEventWithContextEncoder)));
        }
#endif // defined(_WIN32)

        if (!cmdLine.logpath.empty()) {
            fassert(16448, !cmdLine.logWithSyslog);
            string absoluteLogpath = boost::filesystem::absolute(
                    cmdLine.logpath, cmdLine.cwd).string();
            StatusWithRotatableFileWriter writer =
                logger::globalRotatableFileManager()->openFile(absoluteLogpath, cmdLine.logAppend);
            if (!writer.isOK()) {
                return writer.getStatus();
            }
            LogManager* manager = logger::globalLogManager();
            manager->getGlobalDomain()->clearAppenders();
            manager->getGlobalDomain()->attachAppender(
                    MessageLogDomain::AppenderAutoPtr(
                            new RotatableFileAppender<MessageEventEphemeral>(
                                    new MessageEventDetailsEncoder, writer.getValue())));
            manager->getNamedDomain("javascriptOutput")->attachAppender(
                    MessageLogDomain::AppenderAutoPtr(
                            new RotatableFileAppender<MessageEventEphemeral>(
                                    new MessageEventDetailsEncoder, writer.getValue())));
        }

        return Status::OK();
    }

    bool initializeServerGlobalState() {

        Listener::globalTicketHolder.resize( cmdLine.maxConns );

#ifndef _WIN32
        if (!fs::is_directory(cmdLine.socket)) {
            cout << cmdLine.socket << " must be a directory" << endl;
            return false;
        }
#endif

        if (!cmdLine.pidFile.empty()) {
            writePidFile(cmdLine.pidFile);
        }

        if (!cmdLine.keyFile.empty()) {

            if (!setUpSecurityKey(cmdLine.keyFile)) {
                // error message printed in setUpPrivateKey
                return false;
            }

            AuthorizationManager::setAuthEnabled(true);
        }

        return true;
    }

    static void ignoreSignal( int sig ) {}

    void setupCoreSignals() {
#if !defined(_WIN32)
        verify( signal(SIGHUP , ignoreSignal ) != SIG_ERR );
        verify( signal(SIGUSR2, ignoreSignal ) != SIG_ERR );
#endif
    }

}  // namespace mongo
