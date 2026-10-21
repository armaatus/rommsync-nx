#include "rommsync/tico.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "rommsync/config_text.hpp"
#include "rommsync/save_source.hpp"

namespace rommsync::tico {
namespace {

using config::text::LowerAscii;

std::string Join(std::string_view directory, std::string_view name) {
  std::string path(directory);
  if (path.empty() || path.back() != '/') {
    path.push_back('/');
  }
  path.append(name);
  return path;
}

std::string Listed(const std::vector<std::string>& accounts, std::size_t dropped) {
  std::string out;
  for (const std::string& account : accounts) {
    out += out.empty() ? "" : ", ";
    out += account;
  }
  if (dropped > 0) {
    out += " and " + std::to_string(dropped) + " more";
  }
  return out;
}

config::Diagnostic Note(config::Severity severity, std::string message) {
  config::Diagnostic note;
  note.severity = severity;
  note.section = "tico";
  note.key = "account";
  note.message = std::move(message);
  return note;
}

}  // namespace

std::string RootOf(std::string_view folder) {
  if (scan::SourceOf(folder).source != scan::Source::kTico) {
    return {};
  }
  const std::size_t slash = folder.rfind('/');
  if (slash == std::string_view::npos || slash == 0 || slash + 1 == folder.size()) {
    return {};
  }
  return std::string(folder.substr(0, slash));
}

std::string FolderIn(const Resolution& resolved, std::string_view folder) {
  switch (resolved.layout) {
    case Layout::kLegacy:
      return std::string(folder);
    case Layout::kAccount:
      return Join(Join(resolved.users_dir, resolved.uid), folder.substr(folder.rfind('/') + 1));
    case Layout::kUndecided:
      break;
  }
  return {};
}

Resolution Resolve(const config::TicoConfig& tico, fs::FileSystem& files, std::string_view root) {
  Resolution resolved;
  resolved.users_dir = Join(root, kUsersFolder);
  if (tico.account == config::TicoAccount::kLegacy) {
    return resolved;
  }
  const fs::Listing listing = files.List(resolved.users_dir);
  if (listing.error == fs::ListError::kMissing || listing.error == fs::ListError::kNotADirectory) {
    return resolved;  // Tico before 0.8.0, or 0.8.0 that has never had a session
  }

  std::set<std::string> seen;
  for (const fs::Entry& entry : listing.entries) {
    if (entry.is_directory && config::IsTicoUid(entry.name)) {
      seen.insert(LowerAscii(entry.name));
    }
  }
  for (const std::string& account : seen) {
    if (resolved.accounts.size() < kMaxAccounts) {
      resolved.accounts.push_back(account);
    } else {
      ++resolved.accounts_dropped;
    }
  }

  if (tico.account == config::TicoAccount::kNamed) {
    // Named outright, so there is nothing to choose; a folder that is not there
    // is a missing folder like any other, and `Notes` says so.
    resolved.layout = Layout::kAccount;
    resolved.uid = tico.uid;
    return resolved;
  }
  if (!listing.ok() && listing.error != fs::ListError::kTooManyEntries) {
    resolved.layout = Layout::kUndecided;
    resolved.why = resolved.users_dir + " could not be read (" +
                   (listing.message.empty() ? fs::ToString(listing.error) : listing.message) +
                   "), so whose saves are in it is not known; none of them is synced";
    return resolved;
  }
  const std::size_t total = resolved.accounts.size() + resolved.accounts_dropped;
  if (total == 0) {
    // An empty `users/` is a Tico that has written nothing per account yet --
    // and without a session it writes the old layout, so that is where saves are.
    return resolved;
  }
  if (total == 1) {
    resolved.layout = Layout::kAccount;
    resolved.uid = resolved.accounts.front();
    return resolved;
  }
  resolved.layout = Layout::kUndecided;
  resolved.why = "Tico keeps saves for " + std::to_string(total) + " accounts in " +
                 resolved.users_dir +
                 " and [tico] account does not say whose to sync, so none of them is synced";
  return resolved;
}

std::vector<std::string> Roots(const config::Config& config) {
  std::vector<std::string> roots;
  std::set<std::string> seen;
  const auto collect = [&](const std::vector<std::string>& folders) {
    for (const std::string& folder : folders) {
      std::string root = RootOf(folder);
      if (!root.empty() && seen.insert(LowerAscii(root)).second) {
        roots.push_back(std::move(root));
      }
    }
  };
  collect(config.SaveScanDirs());
  if (config.sync.states) {
    collect(config.StateScanDirs());
  }
  return roots;
}

std::vector<config::Diagnostic> Notes(const config::Config& config, fs::FileSystem& files) {
  std::vector<config::Diagnostic> notes;
  for (const std::string& root : Roots(config)) {
    const Resolution resolved = Resolve(config.tico, files, root);
    switch (resolved.layout) {
      case Layout::kLegacy:
        break;
      case Layout::kUndecided:
        notes.push_back(Note(config::Severity::kWarning,
                             resolved.why +
                                 (resolved.accounts.empty()
                                      ? std::string()
                                      : " -- set it to one of " +
                                            Listed(resolved.accounts, resolved.accounts_dropped))));
        break;
      case Layout::kAccount:
        if (config.tico.account == config::TicoAccount::kAuto) {
          notes.push_back(Note(config::Severity::kNotice,
                               "Tico keeps saves per account in " + resolved.users_dir +
                                   "; the only one there, " + resolved.uid + ", is synced"));
        } else if (std::find(resolved.accounts.begin(), resolved.accounts.end(), resolved.uid) ==
                       resolved.accounts.end() &&
                   resolved.accounts_dropped == 0) {
          notes.push_back(Note(
              config::Severity::kWarning,
              "names an account with no folder in " + resolved.users_dir +
                  ", so no Tico save there is synced" +
                  (resolved.accounts.empty()
                       ? std::string()
                       : " -- the accounts there are " +
                             Listed(resolved.accounts, resolved.accounts_dropped))));
        }
        break;
    }
  }
  return notes;
}

}  // namespace rommsync::tico
