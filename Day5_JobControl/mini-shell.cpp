// mini-shell.cpp
// A simple POSIX shell in C++ supporting pipes, redirection, background jobs, and job control.
// Compile with: g++ -std=c++17 -Wall -Wextra -O2 -o minishell mini-shell.cpp

#include <bits/stdc++.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <termios.h>

using namespace std;

enum JobState { RUNNING, STOPPED, DONE };

struct Job {
    int id;
    pid_t pgid;
    string command_line;
    JobState state;
};

static vector<Job> jobs;
static int next_job_id = 1;
static pid_t shell_pgid;
static int shell_terminal;
static struct termios shell_tmodes;

// Forward declarations
void sigchld_handler(int);
void sigint_handler(int);
void sigtstp_handler(int);
void init_signal_handlers();
void add_job(pid_t pgid, const string &cmd, JobState state);
int find_job_index_by_pgid(pid_t pgid);
int find_job_index_by_id(int id);
void remove_done_jobs();
void print_jobs();
void put_job_in_foreground(int idx, bool continue_job);
void put_job_in_background(int idx, bool continue_job);

// Simple tokenizer that respects quotes (single + double)
vector<string> tokenize(const string &line) {
    vector<string> tokens;
    string cur;
    bool in_squote=false, in_dquote=false;
    for (size_t i=0;i<line.size();) {
        char c = line[i];
        if (isspace((unsigned char)c) && !in_squote && !in_dquote) {
            if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
            ++i;
        } else if (c=='\'' && !in_dquote) {
            in_squote = !in_squote;
            ++i;
        } else if (c=='"' && !in_squote) {
            in_dquote = !in_dquote;
            ++i;
        } else if ((c=='|' || c=='>' || c=='<' || c=='&') && !in_squote && !in_dquote) {
            if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
            // support >> as a single token
            if (c=='>' && i+1<line.size() && line[i+1]=='>') { tokens.push_back(">>"); i+=2; }
            else { tokens.push_back(string(1,c)); ++i; }
        } else {
            cur.push_back(c);
            ++i;
        }
    }
    if (!cur.empty()) tokens.push_back(cur);
    return tokens;
}

// Represents a single command in a pipeline
struct Command {
    vector<string> argv;
    string infile;   // "" if none
    string outfile;  // "" if none
    bool append = false;
};

// Parse tokens into pipeline of Commands and background flag
bool parse_pipeline(const vector<string> &tokens, vector<Command> &pipeline, bool &background) {
    pipeline.clear();
    background = false;
    if (tokens.empty()) return false;
    vector<string> cur;
    Command cmd;
    for (size_t i=0;i<tokens.size();) {
        string t = tokens[i];
        if (t=="|") {
            if (cmd.argv.empty()) return false;
            pipeline.push_back(cmd);
            cmd = Command();
            ++i;
        } else if (t=="<") {
            if (i+1>=tokens.size()) return false;
            cmd.infile = tokens[i+1];
            i += 2;
        } else if (t==">" || t==">>") {
            if (i+1>=tokens.size()) return false;
            cmd.outfile = tokens[i+1];
            cmd.append = (t==">>");
            i += 2;
        } else if (t=="&" && i==tokens.size()-1) {
            background = true;
            ++i;
        } else {
            cmd.argv.push_back(t);
            ++i;
        }
    }
    if (!cmd.argv.empty() || !cmd.infile.empty() || !cmd.outfile.empty()) pipeline.push_back(cmd);
    return !pipeline.empty();
}

// Convert vector<string> to char* array for exec
char **make_argv(const vector<string> &v) {
    char **argv = new char*[v.size()+1];
    for (size_t i=0;i<v.size();++i) argv[i] = strdup(v[i].c_str());
    argv[v.size()] = nullptr;
    return argv;
}
void free_argv(char **argv) {
    if (!argv) return;
    for (size_t i=0; argv[i]!=nullptr; ++i) free(argv[i]);
    delete[] argv;
}

// Built-in handling: return true if built-in executed (shell should not fork)
bool handle_builtin(vector<string> &argv) {
    if (argv.empty()) return false;
    string cmd = argv[0];
    if (cmd=="cd") {
        const char *path = nullptr;
        if (argv.size()>=2) path = argv[1].c_str();
        else path = getenv("HOME");
        if (::chdir(path) != 0) {
            perror("cd");
        }
        return true;
    } else if (cmd=="exit") {
        exit(0);
    } else if (cmd=="jobs") {
        remove_done_jobs();
        print_jobs();
        return true;
    } else if (cmd=="fg" || cmd=="bg") {
        if (argv.size()<2) {
            cerr << cmd << ": usage: " << cmd << " %jobid\n";
            return true;
        }
        string s = argv[1];
        if (s.size()>0 && s[0]=='%') s = s.substr(1);
        int id = stoi(s);
        int idx = find_job_index_by_id(id);
        if (idx<0) { cerr << cmd << ": no such job\n"; return true; }
        if (cmd=="fg") put_job_in_foreground(idx, true);
        else put_job_in_background(idx, true);
        return true;
    }
    return false;
}

// Create child process for command, set up stdio redirections. Returns pid.
pid_t spawn_process(Command &cmd, pid_t pgid, int in_fd, int out_fd, bool is_background) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        // Child
        // Put in process group pgid (create if 0)
        if (pgid == 0) pgid = getpid();
        setpgid(0, pgid);

        // If foreground, take terminal
        if (!is_background) {
            tcsetpgrp(shell_terminal, pgid);
        }

        // Restore default signals in child
        signal(SIGINT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);

        // Setup redirections
        if (in_fd != STDIN_FILENO) {
            dup2(in_fd, STDIN_FILENO);
            close(in_fd);
        }
        if (out_fd != STDOUT_FILENO) {
            dup2(out_fd, STDOUT_FILENO);
            close(out_fd);
        }

        // Input file
        if (!cmd.infile.empty()) {
            int fd = open(cmd.infile.c_str(), O_RDONLY);
            if (fd < 0) { perror("open infile"); exit(1); }
            dup2(fd, STDIN_FILENO);
            close(fd);
        }
        // Output file
        if (!cmd.outfile.empty()) {
            int flags = O_WRONLY | O_CREAT | (cmd.append ? O_APPEND : O_TRUNC);
            int fd = open(cmd.outfile.c_str(), flags, 0644);
            if (fd < 0) { perror("open outfile"); exit(1); }
            dup2(fd, STDOUT_FILENO);
            close(fd);
        }

        // Build argv
        char **argv = make_argv(cmd.argv);
        execvp(argv[0], argv);
        perror("execvp");
        free_argv(argv);
        _exit(127);
    } else {
        // Parent
        // set child's pgid
        if (pgid == 0) pgid = pid;
        setpgid(pid, pgid);
    }
    return pid;
}

// Launch a pipeline of commands
void launch_pipeline(vector<Command> &pipeline, bool background, const string &cmdline) {
    size_t n = pipeline.size();
    int prev_fd = STDIN_FILENO;
    vector<int> pipefds;

    pid_t pgid = 0;
    vector<pid_t> pids;

    for (size_t i=0;i<n;i++) {
        int fdpipe[2] = {-1,-1};
        if (i+1 < n) {
            if (pipe(fdpipe) < 0) { perror("pipe"); return; }
        }
        int out_fd = (i+1<n) ? fdpipe[1] : STDOUT_FILENO;
        pid_t pid = spawn_process(pipeline[i], pgid, prev_fd, out_fd, background);
        if (pid < 0) {
            // cleanup fds
            if (fdpipe[0] != -1) close(fdpipe[0]);
            if (fdpipe[1] != -1) close(fdpipe[1]);
            return;
        }
        if (pgid == 0) pgid = pid;
        pids.push_back(pid);

        if (prev_fd != STDIN_FILENO) close(prev_fd);
        if (out_fd != STDOUT_FILENO) close(out_fd);

        if (fdpipe[0] != -1) prev_fd = fdpipe[0];
        else prev_fd = STDIN_FILENO;
    }

    // All children created; set process group and job handling
    add_job(pgid, cmdline, background ? RUNNING : RUNNING);
    int job_idx = find_job_index_by_pgid(pgid);
    if (!background) {
        put_job_in_foreground(job_idx, false);
    } else {
        cout << '[' << jobs[job_idx].id << "] " << pgid << " " << jobs[job_idx].command_line << "\n";
    }
}

// ---------------- Job control functions ----------------

void add_job(pid_t pgid, const string &cmd, JobState state) {
    Job j;
    j.id = next_job_id++;
    j.pgid = pgid;
    j.command_line = cmd;
    j.state = state;
    jobs.push_back(j);
}

int find_job_index_by_pgid(pid_t pgid) {
    for (size_t i=0;i<jobs.size();++i) if (jobs[i].pgid == pgid) return (int)i;
    return -1;
}
int find_job_index_by_id(int id) {
    for (size_t i=0;i<jobs.size();++i) if (jobs[i].id == id) return (int)i;
    return -1;
}

void remove_done_jobs() {
    // reap DONE jobs from jobs vector
    vector<Job> alive;
    for (auto &j : jobs) {
        if (j.state == DONE) {
            // skip
        } else alive.push_back(j);
    }
    jobs.swap(alive);
}

void print_jobs() {
    for (auto &j: jobs) {
        string st = (j.state==RUNNING ? "Running" : (j.state==STOPPED?"Stopped":"Done"));
        cout << '[' << j.id << "] " << st << "\t" << j.command_line << "\n";
    }
}

void put_job_in_foreground(int idx, bool continue_job) {
    if (idx<0 || idx >= (int)jobs.size()) return;
    Job &j = jobs[idx];
    // give terminal control
    tcsetpgrp(shell_terminal, j.pgid);
    if (continue_job) {
        if (kill(-j.pgid, SIGCONT) < 0) perror("kill (SIGCONT)");
    }
    // wait for job
    int status;
    pid_t pid;
    do {
        pid = waitpid(-j.pgid, &status, WUNTRACED);
    } while (pid > 0 && !WIFEXITED(status) && !WIFSIGNALED(status) && !WIFSTOPPED(status));
    if (pid>0) {
        if (WIFSTOPPED(status)) {
            j.state = STOPPED;
            cout << "\n[" << j.id << "] Stopped\t" << j.command_line << "\n";
        } else {
            j.state = DONE;
        }
    }
    // restore shell as terminal owner
    tcsetpgrp(shell_terminal, shell_pgid);
}

void put_job_in_background(int idx, bool continue_job) {
    if (idx<0 || idx >= (int)jobs.size()) return;
    Job &j = jobs[idx];
    if (continue_job) {
        if (kill(-j.pgid, SIGCONT) < 0) perror("kill (SIGCONT)");
    }
    j.state = RUNNING;
    cout << "[" << j.id << "] " << j.pgid << " " << j.command_line << "\n";
}

// ---------------- Signal handlers ----------------

void sigchld_handler(int sig) {
    // Reap terminated children and update job states
    int saved_errno = errno;
    while (true) {
        int status;
        pid_t pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED);
        if (pid <= 0) break;
        // find which job
        pid_t pgid = getpgid(pid);
        int idx = find_job_index_by_pgid(pgid);
        if (idx >= 0) {
            if (WIFSTOPPED(status)) {
                jobs[idx].state = STOPPED;
            } else if (WIFCONTINUED(status)) {
                jobs[idx].state = RUNNING;
            } else if (WIFEXITED(status) || WIFSIGNALED(status)) {
                jobs[idx].state = DONE;
            }
        }
    }
    errno = saved_errno;
}

void sigint_handler(int sig) {
    // forward SIGINT to foreground process group
    pid_t fg = tcgetpgrp(shell_terminal);
    if (fg != shell_pgid) {
        kill(-fg, SIGINT);
    }
}

void sigtstp_handler(int sig) {
    pid_t fg = tcgetpgrp(shell_terminal);
    if (fg != shell_pgid) {
        kill(-fg, SIGTSTP);
    }
}

void init_signal_handlers() {
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, nullptr);

    signal(SIGINT, sigint_handler);
    signal(SIGTSTP, sigtstp_handler);
}

// ---------------- Main REPL ----------------

int main() {
    // Setup shell process group and terminal
    shell_terminal = STDIN_FILENO;
    shell_pgid = getpid();
    // put shell in its own process group
    setpgid(shell_pgid, shell_pgid);
    tcsetpgrp(shell_terminal, shell_pgid);
    tcgetattr(shell_terminal, &shell_tmodes);

    init_signal_handlers();

    string line;
    while (true) {
        // Clean up finished jobs (non-blocking)
        for (auto &j : jobs) {
            if (j.state == DONE) {
                cout << "[" << j.id << "] Done\t" << j.command_line << "\n";
            }
        }
        remove_done_jobs();

        // Print prompt
        char cwd[4096];
        if (getcwd(cwd, sizeof(cwd))!=nullptr) {
            cout << string(cwd) << " $ ";
        } else {
            cout << "$ ";
        }
        cout.flush();

        if (!std::getline(cin, line)) {
            cout << '\n';
            break;
        }
        if (line.empty()) continue;

        // Tokenize
        auto tokens = tokenize(line);
        vector<Command> pipeline;
        bool background = false;
        if (!parse_pipeline(tokens, pipeline, background)) {
            cerr << "Parse error\n";
            continue;
        }

        // If single command and builtin, handle directly
        if (pipeline.size()==1) {
            vector<string> args = pipeline[0].argv;
            if (!pipeline[0].infile.empty() || !pipeline[0].outfile.empty()) {
                // redirection exists: still must fork to apply redirection safely
            } else if (handle_builtin(args)) {
                continue;
            }
        }

        // Launch pipeline
        launch_pipeline(pipeline, background, line);
    }

    return 0;
}
