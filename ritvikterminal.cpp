#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <algorithm>
#include <clocale>
#include <csignal>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <limits.h>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>
using namespace std;
namespace fs = filesystem;
bool cursorVisible = true; 
clock_t prevBlinkTime = 0;
int scrollY = 0;
int scrollX = 0;
int W_width = 1280, w_height = 800;
mutex tabMutex;

struct Line {
  string text;
  bool prompt;
};

struct Job {
  pid_t processGroupId;
  string command;
  string status;
};

struct Tab {
  vector<Line> outputBuffer;
  string inputBuffer;
  string currentDir;
  size_t cursorPosition = 0;
  vector<Job> jobs;
};

vector<Tab> tabs;
int currentTab = 0;
enum ShellMode { NORMAL, SEARCH, AUTOCOMPLETE_SELECTION };
ShellMode shellMode = NORMAL;
string searchTerm;
vector<string> commandHistory;
const size_t MAX_HISTORY = 10000;
const size_t HISTORY_COMMANDS_TO_SHOW = 1000;
string autoComplete;
size_t startPosComplete;
vector<string> completionChoices;
pid_t foregroundProcessGroupId = -1;
string foregroundCommand;
bool inMultiWatch = false;
vector<pid_t> multiWatchPids;
vector<string> multiWatchTempFiles;
vector<string> multiWatchCommands;
vector<streampos> multiWatchFilePositions;
const int TAB_Height = 30;
const int TAB_Width = 150;

string getPrompt(const Tab &tab) {
  if (shellMode == AUTOCOMPLETE_SELECTION) {
    return "Choose [1-" + to_string(completionChoices.size()) + "]: ";
  }
  if (shellMode == SEARCH) {
    return "Enter search term: ";
  }
  if (inMultiWatch) {
    return "[multiWatch active - Press Ctrl+C to stop] ";
  }

  string currentDirectory = tab.currentDir;
  const char *home = getenv("HOME");
  if (home != nullptr && currentDirectory.rfind(home, 0) == 0) {
    currentDirectory.replace(0, strlen(home), "~");
  }
  return "user@system:" + currentDirectory + "$ ";
}

void outputReaderThread(int fileDescriptor, Tab *tab, bool *needsRedraw) {
  char buffer[4096];
  ssize_t count;
  string output;
  while ((count = read(fileDescriptor, buffer, sizeof(buffer) - 1)) > 0) {
    buffer[count] = '\0';
    output += buffer;
  }
  close(fileDescriptor);

  lock_guard<mutex> lock(tabMutex);
  size_t pos = 0;
  while ((pos = output.find('\n')) != string::npos) {
    tab->outputBuffer.push_back({output.substr(0, pos), false});
    output.erase(0, pos + 1);
  }
  if (!output.empty()) {
    tab->outputBuffer.push_back({output, false});
  }
  *needsRedraw = true;
}

void executeCommand(const string &full_cmd, Tab &tab, bool &needsRedraw) {
  auto starts_with = [](const string &s, const string &prefix) {
    return s.size() >= prefix.size() && s.rfind(prefix, 0) == 0;
  };

  vector<string> commands;
  stringstream ss(full_cmd);
  string segment;
  while (getline(ss, segment, '|')) {
    size_t first = segment.find_first_not_of(" \t\n\r");
    if (string::npos != first) {
      size_t last = segment.find_last_not_of(" \t\n\r");
      commands.push_back(segment.substr(first, (last - first + 1)));
    }
  }

  if (commands.empty())
    return;

  if (starts_with(commands[0], "cd")) {
    string cmd_str = commands[0];
    if (cmd_str.length() == 2 || isspace(cmd_str[2])) {
      string path;
      size_t first_space = cmd_str.find_first_of(" \t");
      if (first_space != string::npos) {
        size_t arg_start = cmd_str.find_first_not_of(" \t", first_space);
        if (arg_start != string::npos) {
          path = cmd_str.substr(arg_start);
        }
      }

      if (path.empty() || path == "~") {
        const char *home = getenv("HOME");
        if (home != nullptr) {
          path = home;
        } else {
          return;
        }
      }

      if (chdir(path.c_str()) == 0) {
        tab.currentDir = fs::current_path().string();
      } else {
        tab.outputBuffer.push_back(
            {"cd: " + string(strerror(errno)) + ": " + path, false});
      }
      return;
    }
  }

  int final_output_pipe[2];
  if (pipe(final_output_pipe) == -1) {
    tab.outputBuffer.push_back({"pipe failed", false});
    return;
  }

  pid_t processGroupId = 0;
  int input_fd = STDIN_FILENO;

  for (int i = 0; i < (int)commands.size(); ++i) {
    int pipe_fds[2];
    int output_fd;

    if (i < (int)commands.size() - 1) {
      if (pipe(pipe_fds) == -1) {
        perror("pipe");
        return;
      }
      output_fd = pipe_fds[1];
    } else {
      output_fd = final_output_pipe[1];
    }

    pid_t pid = fork();
    if (pid == 0) {
      signal(SIGINT, SIG_DFL);
      signal(SIGTSTP, SIG_DFL);

      if (input_fd != STDIN_FILENO) {
        dup2(input_fd, STDIN_FILENO);
        close(input_fd);
      }
      if (output_fd != STDOUT_FILENO) {
        dup2(output_fd, STDOUT_FILENO);
        if (i == (int)commands.size() - 1)
          dup2(output_fd, STDERR_FILENO);
        close(output_fd);
      }

      if (i < (int)commands.size() - 1)
        close(pipe_fds[0]);
      close(final_output_pipe[0]);
      close(final_output_pipe[1]);

      chdir(tab.currentDir.c_str());

      const string &cmd = commands[i];
      if (cmd.empty())
        _exit(0);
      execl("/bin/sh", "sh", "-c", cmd.c_str(), (char *)NULL);
      perror("execl failed");
      _exit(127);
    } else if (pid > 0) {
      if (i == 0)
        processGroupId = pid;
      setpgid(pid, processGroupId);

      if (input_fd != STDIN_FILENO)
        close(input_fd);
      if (i < (int)commands.size() - 1) {
        close(pipe_fds[1]);
        input_fd = pipe_fds[0];
      }
    } else {
      perror("fork");
      return;
    }
  }

  foregroundProcessGroupId = processGroupId;
  foregroundCommand = full_cmd;

  close(final_output_pipe[1]);

  thread(outputReaderThread, final_output_pipe[0], &tab, &needsRedraw).detach();
}

void handleAutocomplete(Tab &tab) {
  size_t cursor_pos = tab.cursorPosition;
  if (cursor_pos == 0)
    return;

  size_t word_start = tab.inputBuffer.rfind(' ', cursor_pos - 1);
  word_start = (word_start == string::npos) ? 0 : word_start + 1;

  string prefix = tab.inputBuffer.substr(word_start, cursor_pos - word_start);
  vector<string> matches;

  try {
    for (const auto &entry : fs::directory_iterator(tab.currentDir)) {
      string filename = entry.path().filename().string();
      if (filename.rfind(prefix, 0) == 0) {
        matches.push_back(filename);
      }
    }
  } catch (const fs::filesystem_error &e) {
    return;
  }

  if (matches.empty())
    return;

  if (matches.size() == 1) {
    string completion = matches[0];
    tab.inputBuffer.replace(word_start, prefix.length(), completion);
    if (fs::is_directory(fs::path(tab.currentDir) / completion)) {
      tab.inputBuffer += "/";
    } else {
      tab.inputBuffer += " ";
    }
    tab.cursorPosition = tab.inputBuffer.length();
  } else {
    string lcp = matches[0];
    for (size_t i = 1; i < matches.size(); ++i) {
      size_t j = 0;
      while (j < lcp.length() && j < matches[i].length() && lcp[j] == matches[i][j]) {
        j++;
      }
      lcp = lcp.substr(0, j);
    }

    if (lcp.length() > prefix.length()) {
      tab.inputBuffer.replace(word_start, prefix.length(), lcp);
      tab.cursorPosition = word_start + lcp.length();
    } else {
      shellMode = AUTOCOMPLETE_SELECTION;
      autoComplete = tab.inputBuffer;
      startPosComplete = word_start;
      completionChoices = matches;

      string choice_prompt = "\n";
      for (size_t i = 0; i < matches.size(); ++i) {
        choice_prompt += to_string(i + 1) + ". " + matches[i] + "\t";
      }
      tab.outputBuffer.push_back({choice_prompt, false});
      tab.inputBuffer.clear();
      tab.cursorPosition = 0;
    }
  }
}

string getHistoryFilePath() {
  const char *home = getenv("HOME");
  return (home != nullptr) ? string(home) + "/.my_shell_history" : ".my_shell_history";
}

void loadCommandHistory() {
  ifstream historyFile(getHistoryFilePath());
  if (historyFile.is_open()) {
    string line;
    while (getline(historyFile, line)) {
      commandHistory.push_back(line);
    }
  }
}

void saveCommandHistory() {
  ofstream historyFile(getHistoryFilePath(), ios::trunc);
  if (historyFile.is_open()) {
    size_t start = (commandHistory.size() > MAX_HISTORY)? commandHistory.size() - MAX_HISTORY: 0;
    for (size_t i = start; i < commandHistory.size(); ++i) {
      historyFile << commandHistory[i] << endl;
    }
  }
}

void addToHistory(const string &command) {
  if (command.empty() ||
      (!commandHistory.empty() && commandHistory.back() == command)) {
    return;
  }
  commandHistory.push_back(command);
  if (commandHistory.size() > MAX_HISTORY) {
    commandHistory.erase(commandHistory.begin());
  }
}

string longestCommonSubstring(const string &str1, const string &str2) {
  if (str1.empty() || str2.empty())
    return "";
  vector<vector<int>> dp(str1.size() + 1, vector<int>(str2.size() + 1, 0));
  int maxLength = 0;
  int endIndex = 0;
  for (size_t i = 1; i <= str1.size(); ++i) {
    for (size_t j = 1; j <= str2.size(); ++j) {
      if (str1[i - 1] == str2[j - 1]) {
        dp[i][j] = dp[i - 1][j - 1] + 1;
        if (dp[i][j] > maxLength) {
          maxLength = dp[i][j];
          endIndex = i;
        }
      }
    }
  }
  if (maxLength == 0)
    return "";
  return str1.substr(endIndex - maxLength, maxLength);
}

vector<string> parseMultiWatchCommands(string args) {
  vector<string> commands;
  if (!args.empty()) {
    size_t first = args.find_first_not_of(" \t\n\r");
    if (first != string::npos)
      args = args.substr(first);
    if (!args.empty() && args.front() == '[')
      args.erase(0, 1);
    size_t last = args.find_last_not_of(" \t\n\r");
    if (last != string::npos)
      args.erase(last + 1);
    if (!args.empty() && args.back() == ']')
      args.pop_back();
  }
  stringstream ss(args);
  string cmd;
  while (getline(ss, cmd, ',')) {
    if (!cmd.empty()) {
      size_t first = cmd.find_first_not_of(" \t\n\r\"");
      if (first != string::npos) {
        size_t last = cmd.find_last_not_of(" \t\n\r\"");
        cmd = cmd.substr(first, (last - first + 1));
      }
    }
    if (!cmd.empty())
      commands.push_back(cmd);
  }
  return commands;
}

void startMultiWatch(const vector<string> &commands, Tab &tab) {
  inMultiWatch = true;
  for (const auto &cmd : commands) {
    pid_t pid = fork();
    if (pid == 0) {
      pid_t child_pid = getpid();
      string temp_file ="/tmp/.term_multiwatch_" + to_string(child_pid) + ".tmp";
      int fd = open(temp_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (fd < 0) {
        perror("multiWatch: open temp file failed");
        _exit(1);
      }
      dup2(fd, STDOUT_FILENO);
      dup2(fd, STDERR_FILENO);
      close(fd);
      while (true) {
        pid_t exec_pid = fork();
        if (exec_pid == 0) {
          execl("/bin/sh", "sh", "-c", cmd.c_str(), (char *)NULL);
          perror("multiWatch: execl failed");
          _exit(127);
        } else if (exec_pid > 0) {
          waitpid(exec_pid, NULL, 0);
          sleep(2);
        } else {
          perror("multiWatch: fork failed");
        }
      }
      _exit(0);
    } else if (pid > 0) {
      multiWatchPids.push_back(pid);
      multiWatchCommands.push_back(cmd);
      string temp_file = "/tmp/.term_multiwatch_" + to_string(pid) + ".tmp";
      multiWatchTempFiles.push_back(temp_file);
      multiWatchFilePositions.push_back(0);
    } else {
      perror("multiWatch: fork failed");
    }
  }
}

void checkMultiWatchOutput(Tab &tab, bool &needsRedraw, XFontSet fontSet) {
  for (size_t i = 0; i < multiWatchTempFiles.size(); ++i) {
    ifstream file(multiWatchTempFiles[i]);
    if (file.is_open()) {
      file.seekg(0, ios::end);
      streampos current_size = file.tellg();
      if (current_size > multiWatchFilePositions[i]) {
        file.seekg(multiWatchFilePositions[i]);
        string new_output;
        new_output.assign(istreambuf_iterator<char>(file),istreambuf_iterator<char>());
        if (!new_output.empty()) {
          time_t now = time(0);
          char *dt = ctime(&now);
          string timestamp(dt);
          timestamp.pop_back();
          tab.outputBuffer.push_back(
              {"\"" + multiWatchCommands[i] + "\", current time: " + timestamp,
               true});
          tab.outputBuffer.push_back(
              {"----------------------------------------------------", false});
          stringstream ss(new_output);
          string line;
          while (getline(ss, line))
            tab.outputBuffer.push_back({line, false});
          tab.outputBuffer.push_back(
              {"----------------------------------------------------", false});
          multiWatchFilePositions[i] = file.tellg();
          const int lineHeight =XExtentsOfFontSet(fontSet)->max_logical_extent.height;
          int visibleLines = (w_height > TAB_Height)? (w_height - TAB_Height) / lineHeight : 0;
          scrollY = max(0, (int)tab.outputBuffer.size() - visibleLines + 1);
          needsRedraw = true;
        }
      }
    }
  }
}

void stopMultiWatch(Tab &tab) {
  if (!inMultiWatch)
    return;
  for (pid_t pid : multiWatchPids) {
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
  }
  for (const auto &file : multiWatchTempFiles)
    fs::remove(file);
  multiWatchPids.clear();
  multiWatchTempFiles.clear();
  multiWatchCommands.clear();
  multiWatchFilePositions.clear();
  inMultiWatch = false;
  tab.outputBuffer.push_back({"\n[multiWatch terminated by user]", true});
}

void updateJobStatus(Tab &tab, bool &needsRedraw) {
  int status;
  pid_t pid;

  while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {
    if (WIFEXITED(status) || WIFSIGNALED(status)) {
      if (pid == foregroundProcessGroupId) {
        foregroundProcessGroupId = -1;
        needsRedraw = true;
      }
    } else if (WIFSTOPPED(status)) {
      if (pid == foregroundProcessGroupId) {
        tab.jobs.push_back({foregroundProcessGroupId, foregroundCommand, "Stopped"});
        tab.outputBuffer.push_back({"\n[" + to_string(tab.jobs.size()) + "]+  Stopped\t" + foregroundCommand,false});
        foregroundProcessGroupId = -1;
        needsRedraw = true;
      }
    }
  }
}

int main() {
  if (setlocale(LC_ALL, "") == NULL) {
    cerr << "Warning: Cannot set locale." << endl;
  } else {
    cerr << "Locale set to: " << setlocale(LC_ALL, NULL) << endl;
  }

  loadCommandHistory();

  signal(SIGINT, SIG_IGN);
  signal(SIGTSTP, SIG_IGN);

  tabs.push_back(Tab());
  tabs[0].currentDir = fs::current_path().string();

  Display *display = XOpenDisplay(nullptr);
  if (!display) {
    cerr << "Cannot open display" << endl;
    return 1;
  }

  if (!XSupportsLocale()) {
    cerr << "Warning: X does not support current locale." << endl;
  }
  XSetLocaleModifiers("");

  int screen = DefaultScreen(display);
  Window window = XCreateSimpleWindow(display, RootWindow(display, screen), 10, 10, W_width, w_height, 
        1, BlackPixel(display, screen), BlackPixel(display, screen));

  XSelectInput(display, window,ExposureMask | KeyPressMask | ButtonPressMask |StructureNotifyMask);
  XStoreName(display, window, "My Terminal");
  XMapWindow(display, window);

  GC gc = XCreateGC(display, window, 0, nullptr); 

  XFontSet fontSet; 
  int missing_char_count;
  char **missing_char_list;
  char *def_string;
  const char *font_names ="-*-*-medium-r-normal--*-150-*-*-*-*-iso10646-1,""-*-*-medium-r-normal--*-120-*-*-*-*-*-*,""fixed"; 
  fontSet = XCreateFontSet(display, font_names, &missing_char_list, &missing_char_count, &def_string); 

  if (missing_char_count > 0) {
    cerr << "Warning: Missing charsets for the following:" << endl;
    for (int i = 0; i < missing_char_count; i++)
      cerr << "  " << missing_char_list[i] << endl;
    XFreeStringList(missing_char_list);
  }
  if (!fontSet) {
    cerr << "Fatal: Cannot create any font set." << endl;
    XCloseDisplay(display);
    return 1;
  }
  XFontSetExtents *font_extents = XExtentsOfFontSet(fontSet);
  const int lineHeight = font_extents->max_logical_extent.height;

  XIM xim = XOpenIM(display, nullptr, nullptr, nullptr);
  if (!xim) {
    cerr << "XOpenIM failed." << endl;
    XSetLocaleModifiers("@im=none");
    xim = XOpenIM(display, nullptr, nullptr, nullptr);
  }
  XIC xic = XCreateIC(xim, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow, window, XNFocusWindow, window, NULL);
  if (!xic) {
    cerr << "XCreateIC failed." << endl;
  } else {
    XSetICFocus(xic);
  }

  Colormap colormap = DefaultColormap(display, screen);
  XColor green, white;
  XParseColor(display, colormap, "#00FF00", &green);
  XAllocColor(display, colormap, &green);
  XParseColor(display, colormap, "#FFFFFF", &white);
  XAllocColor(display, colormap, &white);

  bool needsRedraw = true;
  bool isRunning = true;

  while (isRunning) {
    while (isRunning && XPending(display)) {
      XEvent event;
      XNextEvent(display, &event);

      if (XFilterEvent(&event, window))
        continue;

      switch (event.type) {
      case Expose:
        needsRedraw = true;
        break;

      case ConfigureNotify:
        if (event.xconfigure.width != W_width || event.xconfigure.height != w_height) {
          W_width = event.xconfigure.width;
          w_height = event.xconfigure.height;
          needsRedraw = true;
        }
        break;

      case KeyPress: {
        Tab &tab = tabs[currentTab];
        KeySym keysym = NoSymbol;
        cursorVisible = true;
        prevBlinkTime = clock();

        KeySym raw_keysym = XLookupKeysym(&event.xkey, 0);

        if ((event.xkey.state & ControlMask) && raw_keysym == XK_c) {
          if (shellMode == SEARCH) {
            shellMode = NORMAL;
            searchTerm.clear();
          } else if (shellMode == AUTOCOMPLETE_SELECTION) {
            shellMode = NORMAL;
            tab.inputBuffer = autoComplete;
            tab.cursorPosition = tab.inputBuffer.length();
          } else if (inMultiWatch) {
            stopMultiWatch(tab);
          } else if (foregroundProcessGroupId != -1) {
            kill(-foregroundProcessGroupId, SIGINT);
          }
          needsRedraw = true;
          continue;
        }
        if ((event.xkey.state & ControlMask) && raw_keysym == XK_z) {
          if (foregroundProcessGroupId != -1)
            kill(-foregroundProcessGroupId, SIGTSTP);
          needsRedraw = true;
          continue;
        }
        if ((event.xkey.state & ControlMask) && raw_keysym == XK_a) {
          tab.cursorPosition = 0;
          needsRedraw = true;
          continue;
        }
        if ((event.xkey.state & ControlMask) && raw_keysym == XK_e) {
          tab.cursorPosition = tab.inputBuffer.length();
          needsRedraw = true;
          continue;
        }
        if ((event.xkey.state & ControlMask) && raw_keysym == XK_r) {
          shellMode = SEARCH;
          searchTerm.clear();
          needsRedraw = true;
          continue;
        }

        if (inMultiWatch)
          continue;

        if (raw_keysym == XK_Tab) {
          if (shellMode == NORMAL && !tab.inputBuffer.empty()) {
            handleAutocomplete(tab);
            needsRedraw = true;
          }
          continue;
        }

        if ((event.xkey.state & ControlMask) && raw_keysym == XK_t) {
          tabs.push_back(Tab());
          currentTab = tabs.size() - 1;
          tabs[currentTab].currentDir = fs::current_path().string();
          needsRedraw = true;
          continue;
        }
        if ((event.xkey.state & ControlMask) && raw_keysym == XK_Tab) {
          currentTab = (currentTab + 1) % tabs.size();
          needsRedraw = true;
          continue;
        }

        char buffer[32];
        Status status;
        int len = Xutf8LookupString(xic, &event.xkey, buffer, sizeof(buffer) - 1, &keysym, &status);
        buffer[len] = '\0';

        if (shellMode == SEARCH) {
          if (keysym == XK_Return) {
            {
              lock_guard<mutex> lock(tabMutex);
              tab.outputBuffer.push_back({getPrompt(tab) + searchTerm, true});
              bool found = false;

              for (auto it = commandHistory.rbegin();
                   it != commandHistory.rend(); ++it) {
                if (*it == searchTerm) {
                  tab.outputBuffer.push_back({*it, false});
                  found = true;
                  break;
                }
              }

              if (!found) {
                vector<string> bestMatches;
                size_t maxLcsLen = 0;
                for (auto it = commandHistory.rbegin();
                     it != commandHistory.rend(); ++it) {
                  size_t lcsLen =
                      longestCommonSubstring(*it, searchTerm).length();
                  if (lcsLen > maxLcsLen) {
                    maxLcsLen = lcsLen;
                    bestMatches.clear();
                    bestMatches.push_back(*it);
                  } else if (lcsLen > 0 && lcsLen == maxLcsLen) {
                    bestMatches.push_back(*it);
                  }
                }
                if (maxLcsLen > 2) {
                  for (const auto &match : bestMatches)
                    tab.outputBuffer.push_back({match, false});
                  found = true;
                }
              }
              if (!found)
                tab.outputBuffer.push_back({"No match for search term in history", false});
            }

            shellMode = NORMAL;
            searchTerm.clear();
            scrollY = tab.outputBuffer.size();
            needsRedraw = true;
          } else if (keysym == XK_BackSpace) {
            if (!searchTerm.empty()) {
              size_t i = searchTerm.length();
              while (--i > 0 && (searchTerm[i] & 0xC0) == 0x80);
              searchTerm.erase(i);
            }
            needsRedraw = true;
          } else if (status == XLookupChars || status == XLookupBoth) {
            searchTerm += buffer;
            needsRedraw = true;
          }
        } else if (shellMode == AUTOCOMPLETE_SELECTION) {
          if (keysym == XK_Return) {
            try {
              int choice = stoi(tab.inputBuffer);
              if (choice > 0 && (size_t)choice <= completionChoices.size()) {
                string selected = completionChoices[choice - 1];
                tab.inputBuffer = autoComplete.substr(0, startPosComplete) + selected;
                if (fs::is_directory(fs::path(tab.currentDir) / selected)) {
                  tab.inputBuffer += "/";
                } else {
                  tab.inputBuffer += " ";
                }
                tab.cursorPosition = tab.inputBuffer.length();
              }
            } catch (const std::invalid_argument &ia) {
              tab.inputBuffer = autoComplete;
              tab.cursorPosition = tab.inputBuffer.length();
            }
            shellMode = NORMAL;
            needsRedraw = true;
          } else if (keysym == XK_BackSpace) {
            if (!tab.inputBuffer.empty())
              tab.inputBuffer.pop_back();
            needsRedraw = true;
          } else if (isdigit(buffer[0])) {
            tab.inputBuffer += buffer;
            needsRedraw = true;
          }
        } else {
          if (keysym == XK_Left) {
            if (tab.cursorPosition > 0) {
              size_t i = tab.cursorPosition;
              while (--i > 0 && (tab.inputBuffer[i] & 0xC0) == 0x80) {
              }
              tab.cursorPosition = i;
            }
            needsRedraw = true;
          } else if (keysym == XK_Right) {
            if (tab.cursorPosition < tab.inputBuffer.length()) {
              size_t i = tab.cursorPosition + 1;
              while (i < tab.inputBuffer.length() &&(tab.inputBuffer[i] & 0xC0) == 0x80)
                i++;
              tab.cursorPosition = i;
            }
            needsRedraw = true;
          } else if (keysym == XK_Return) {
            if (!tab.inputBuffer.empty() && tab.inputBuffer.back() == '\\') {
              tab.inputBuffer.pop_back();
              tab.inputBuffer += '\n';
              tab.cursorPosition = tab.inputBuffer.length();
              needsRedraw = true;
            } else {
              int commandLineIndex = 0;
              if (!tab.inputBuffer.empty()) {
                addToHistory(tab.inputBuffer);
                string input = tab.inputBuffer;
                size_t first = input.find_first_not_of(" \t\n\r");
                string trimmed_input =(string::npos != first) ? input.substr(first,(input.find_last_not_of(" \t\n\r") - first + 1)): "";
                if (trimmed_input == "exit") {
                  isRunning = false;
                  continue;
                }
                if (trimmed_input == "clear") {
                  lock_guard<mutex> lock(tabMutex);
                  tab.outputBuffer.clear();
                  tab.inputBuffer.clear();
                  tab.cursorPosition = 0;
                  scrollY = 0;
                  needsRedraw = true;
                  continue;
                }
                if (trimmed_input == "history") {
                  lock_guard<mutex> lock(tabMutex);
                  size_t start = (commandHistory.size() >HISTORY_COMMANDS_TO_SHOW)? commandHistory.size() - HISTORY_COMMANDS_TO_SHOW : 0;
                  for (size_t i = start; i < commandHistory.size(); ++i) {
                    tab.outputBuffer.push_back({to_string(i + 1) + "  " + commandHistory[i], false});
                  }
                  scrollY = tab.outputBuffer.size();
                  needsRedraw = true;
                } else {
                  commandLineIndex = tab.outputBuffer.size();
                  {
                    lock_guard<mutex> lock(tabMutex);
                    tab.outputBuffer.push_back({getPrompt(tab) + tab.inputBuffer, true});
                  }
                  if (trimmed_input.rfind("multiWatch", 0) == 0) {
                    string args =trimmed_input.substr(string("multiWatch").length());
                    vector<string> commands = parseMultiWatchCommands(args);
                    if (!commands.empty())
                      startMultiWatch(commands, tab);
                    else
                      tab.outputBuffer.push_back({"multiWatch: Invalid arguments", false});
                  } else {
                    executeCommand(tab.inputBuffer, tab, needsRedraw);
                  }
                  scrollY = commandLineIndex;
                }
              }
              tab.inputBuffer.clear();
              tab.cursorPosition = 0;
              needsRedraw = true;
            }
          } else if (keysym == XK_BackSpace) {
            if (tab.cursorPosition > 0) {
              size_t end_pos = tab.cursorPosition;
              size_t start_pos = end_pos;
              while (--start_pos > 0 && (tab.inputBuffer[start_pos] & 0xC0) == 0x80) {}
              tab.inputBuffer.erase(start_pos, end_pos - start_pos);
              tab.cursorPosition = start_pos;
            }
            needsRedraw = true;
          } else if (status == XLookupChars || status == XLookupBoth) {
            tab.inputBuffer.insert(tab.cursorPosition, buffer);
            tab.cursorPosition += strlen(buffer);
            needsRedraw = true;
          }
        }
        break;
      }

      case ButtonPress:
        if (event.xbutton.y < TAB_Height) {
          int clickedTab = event.xbutton.x / TAB_Width;
          if (clickedTab >= 0 && clickedTab < (int)tabs.size()) {
            currentTab = clickedTab;
            needsRedraw = true;
          }
        } else if (event.xbutton.button == Button4) {
          scrollY = max(0, scrollY - 2);
          needsRedraw = true;
        } else if (event.xbutton.button == Button5) {
          scrollY += 2;
          needsRedraw = true;
        }
        break;
      }
    }

    {
      lock_guard<mutex> lock(tabMutex);
      if (inMultiWatch)
        checkMultiWatchOutput(tabs[currentTab], needsRedraw, fontSet);
      updateJobStatus(tabs[currentTab], needsRedraw);
    }

    if ((clock() - prevBlinkTime) * 1000 / CLOCKS_PER_SEC > 500) {
      cursorVisible = !cursorVisible;
      needsRedraw = true;
      prevBlinkTime = clock();
    }

    if (needsRedraw) {
      XClearWindow(display, window);
      Tab &tab = tabs[currentTab];

      for (int i = 0; i < (int)tabs.size(); i++) {
        XSetForeground(display, gc,(i == currentTab) ? green.pixel : white.pixel);
        XDrawRectangle(display, window, gc, i * TAB_Width, 0, TAB_Width,TAB_Height);
        string label = "Tab " + to_string(i + 1);
        Xutf8DrawString(display, window, fontSet, gc, i * TAB_Width + 10, 20,label.c_str(), label.length());
      }

      int visibleLines = (w_height > TAB_Height)? (w_height - TAB_Height) / lineHeight : 0;
      int totalLines = tab.outputBuffer.size();

      bool promptVisible = foregroundProcessGroupId == -1 && !inMultiWatch;
      if (totalLines >= visibleLines && visibleLines > 0 && promptVisible) {
        visibleLines--;
      }

      if (totalLines <= visibleLines) {
        scrollY = 0;
      } else if (scrollY > totalLines - visibleLines) {
        scrollY = totalLines - visibleLines;
      }
      scrollY = max(0, scrollY);

      int startLine = scrollY;
      int endLine = min(totalLines, startLine + visibleLines);
      int y = TAB_Height + 20;
      for (int i = startLine; i < endLine; i++) {
        const Line &lineObj = tab.outputBuffer[i];
        XSetForeground(display, gc,lineObj.prompt ? green.pixel : white.pixel);
        if (scrollX < (int)lineObj.text.size()) {
          string visible = lineObj.text.substr(scrollX);
          Xutf8DrawString(display, window, fontSet, gc, 10, y, visible.c_str(),visible.length());
        }
        y += lineHeight;
      }

      if (promptVisible) {
        int promptY = y;
        string promptStr = getPrompt(tab);
        XSetForeground(display, gc, green.pixel);
        Xutf8DrawString(display, window, fontSet, gc, 10, promptY,promptStr.c_str(), promptStr.length());

        XRectangle ink_rect, logical_rect;
        Xutf8TextExtents(fontSet, promptStr.c_str(), promptStr.length(),&ink_rect, &logical_rect);
        int promptWidth = logical_rect.width;

        string *stringToDraw = &tab.inputBuffer;
        if (shellMode == SEARCH)
          stringToDraw = &searchTerm;

        XSetForeground(display, gc, white.pixel);
        Xutf8DrawString(display, window, fontSet, gc, 10 + promptWidth, promptY,stringToDraw->c_str(), stringToDraw->length());

        if (cursorVisible) {
          size_t cursorPos =
              (shellMode == NORMAL || shellMode == AUTOCOMPLETE_SELECTION)? tab.cursorPosition: stringToDraw->length();
          string preCursorStr = stringToDraw->substr(0, cursorPos);
          Xutf8TextExtents(fontSet, preCursorStr.c_str(), preCursorStr.length(),&ink_rect, &logical_rect);
          int preCursorWidth = logical_rect.width;
          int cursorX = 10 + promptWidth + preCursorWidth;
          int cursorY = promptY + font_extents->max_logical_extent.y;
          XFillRectangle(display, window, gc, cursorX, cursorY, 2, lineHeight);
        }
      }
      needsRedraw = false;
    }
    usleep(20000);
  }

  if (inMultiWatch)
    stopMultiWatch(tabs[currentTab]);
  saveCommandHistory();

  if (xic)
    XDestroyIC(xic);
  if (xim)
    XCloseIM(xim);
  if (fontSet)
    XFreeFontSet(display, fontSet);
  XFreeGC(display, gc);
  XDestroyWindow(display, window);
  XCloseDisplay(display);
  return 0;
}