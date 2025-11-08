#include <bits/stdc++.h>
#include <unistd.h>
#include <sys/wait.h>
using namespace std;

vector<string> tokenize(const string &line) {
    vector<string> tokens;
    string token;
    istringstream iss(line);
    while (iss >> token) tokens.push_back(token);
    return tokens;
}

int main() {
    string line;
    cout << "Custom Shell (Day 3) — Foreground & Background Processes\n";
    while (true) {
        cout << "$ ";
        getline(cin, line);
        if (line == "exit") break;
        if (line.empty()) continue;

        auto tokens = tokenize(line);
        if (tokens.empty()) continue;

        bool background = false;
        if (tokens.back() == "&") {
            background = true;
            tokens.pop_back();
        }

        vector<char*> args;
        for (auto &t : tokens) args.push_back(const_cast<char*>(t.c_str()));
        args.push_back(nullptr);

        pid_t pid = fork();
        if (pid == 0) {
            execvp(args[0], args.data());
            perror("execvp");
            exit(1);
        } else if (pid > 0) {
            if (!background) {
                waitpid(pid, nullptr, 0);
            } else {
                cout << "Started background process PID: " << pid << "\n";
            }
        } else {
            perror("fork");
        }
    }
    return 0;
}
