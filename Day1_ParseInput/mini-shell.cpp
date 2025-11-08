#include <bits/stdc++.h>
using namespace std;

// Tokenize user input into command and arguments
vector<string> tokenize(const string &line) {
    vector<string> tokens;
    string token;
    istringstream iss(line);
    while (iss >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

int main() {
    string line;
    cout << "Simple Shell (Day 1) — Type 'exit' to quit\n";
    while (true) {
        cout << "$ ";
        getline(cin, line);
        if (line == "exit") break;
        if (line.empty()) continue;

        auto tokens = tokenize(line);
        cout << "Parsed tokens:\n";
        for (auto &t : tokens)
            cout << "  " << t << "\n";
    }
    return 0;
}
