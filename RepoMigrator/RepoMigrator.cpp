#include <iostream>
#include <fstream>
#include <string>
#include <filesystem>
#include <git2.h>
#include <windows.h>
#include <wincred.h>
#pragma comment(lib, "Credui.lib")
#pragma comment(lib, "git2.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "rpcrt4.lib")
#pragma comment(lib, "crypt32.lib")

using namespace std;
namespace fs = std::filesystem;

ofstream logFile;

void log(const string& message, bool isError = false) {
	if (isError) {
		cerr << message << endl;
	}
	else {
		cout << message << endl;
	}
	if (logFile.is_open()) {
		logFile << message << "\n";
		logFile.flush();
	}
}

// Displays the current libgit2 error
void printGitError(const string& context) {
	const git_error* e = git_error_last();
	string msg = "[ERROR] " + context;
	if (e && e->message) {
		msg += " : " + string(e->message);
	}
	log(msg, true);
}

int exitWithError(const string& message, git_remote* remote, git_repository* repo) {
	log("[ERROR] " + message, true);
	log("Program terminated with error. Check git_transfer.log for details.", true);
	if (remote) git_remote_free(remote);
	if (repo)   git_repository_free(repo);
	git_libgit2_shutdown();
	if (logFile.is_open()) logFile.close();
	return 1;
}

// Checks if a branch exists in the repository
bool branchExists(git_repository* repo, const string& branchName) {
	git_reference* ref = nullptr;
	int result = git_branch_lookup(&ref, repo, branchName.c_str(), GIT_BRANCH_LOCAL);
	if (ref) git_reference_free(ref);
	return result == 0;
}

// Saves credentials in Windows Manager
void saveCredentials(const string& target, const string& username, const string& password) {
	CREDENTIALA cred = {};
	cred.Type = CRED_TYPE_GENERIC;
	cred.TargetName = const_cast<LPSTR>(target.c_str());
	cred.UserName = const_cast<LPSTR>(username.c_str());
	cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(password.c_str()));
	cred.CredentialBlobSize = static_cast<DWORD>(password.size());
	cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
	CredWriteA(&cred, 0);
}

// Read credentials from Windows manager
bool loadCredentials(const string& target, string& username, string& password) {
	PCREDENTIALA cred = nullptr;
	if (CredReadA(target.c_str(), CRED_TYPE_GENERIC, 0, &cred)) {
		username = cred->UserName ? cred->UserName : "";
		password = string(reinterpret_cast<char*>(cred->CredentialBlob), cred->CredentialBlobSize);
		CredFree(cred);
		return true;
	}
	return false;
}

int credentialsCallback(git_credential** out, const char* url, const char* username_from_url,
	unsigned int allowed_types, void* payload) {

	// URL-based storage key (e.g., "git_transfer:https://github.com/user/repo.git")
	string target = string("git_transfer:") + url;

	string username, password;

	if (loadCredentials(target, username, password)) {
		log("Credentials found in Windows Manager for: " + string(url));
	}
	else {
		log("Authentication required for: " + string(url));

		if (username_from_url && strlen(username_from_url) > 0) {
			username = username_from_url;
			log("Username: " + username);
		}
		else {
			cout << "Username: ";
			getline(cin, username);
			log("Username entered: " + username);
		}

		cout << "Token / Password: ";
		getline(cin, password);
		log("Password entered (hidden)");

		saveCredentials(target, username, password);
		log("Credentials saved in Windows Manager.");
	}

	return git_credential_userpass_plaintext_new(out, username.c_str(), password.c_str());
}

int main() {
	//log file
	logFile.open("git_transfer.log", ios::out | ios::trunc);
	if (!logFile.is_open()) {
		cerr << "[WARNING] Could not create log file." << endl;
	}

	log("=== git_transfer started ===");

	string repoSource;
	string repoDestination;

	cout << "Enter the full path to the source repository (local): ";
	getline(cin, repoSource);
	log("Source repo: " + repoSource);

	cout << "Enter the URL (.git) of the destination repository: ";
	getline(cin, repoDestination);
	log("Destination repo: " + repoDestination);

	if (!fs::exists(repoSource)) {
		log("[ERROR] The source folder does not exist: " + repoSource, true);
		if (logFile.is_open()) logFile.close();
		return 1;
	}

	git_libgit2_init();
	log("libgit2 initialized: v" + to_string(LIBGIT2_VER_MAJOR) + "." +
		to_string(LIBGIT2_VER_MINOR) + "." + to_string(LIBGIT2_VER_REVISION));

	git_repository* repo = nullptr;

	log("\n - Opening the local repo");

	if (git_repository_open(&repo, repoSource.c_str()) != 0) {
		printGitError("Unable to open the repository");
		return exitWithError("git_repository_open failed", nullptr, nullptr);
	}

	log("Repo opened: " + repoSource);

	log("\n - Step 1: Detecting the local main branch");

	string localMainBranch;

	if (branchExists(repo, "main")) {
		localMainBranch = "main";
	}
	else if (branchExists(repo, "master")) {
		localMainBranch = "master";
	}
	else {
		// Fallback: current branch
		git_reference* headRef = nullptr;
		if (git_repository_head(&headRef, repo) == 0) {
			localMainBranch = git_reference_shorthand(headRef);
			git_reference_free(headRef);
			log("No main/master branch found, using current branch as fallback.");
		}
	}

	if (localMainBranch.empty()) {
		return exitWithError("Unable to detect a local main branch.", nullptr, repo);
	}

	log("Local main branch: " + localMainBranch);

	log("\n - Step 2: Remote configuration");

	string oldRemoteUrl;
	{
		git_remote* existingRemote = nullptr;
		if (git_remote_lookup(&existingRemote, repo, "origin") == 0) {
			const char* url = git_remote_url(existingRemote);
			if (url) oldRemoteUrl = url;
			git_remote_free(existingRemote);
			log("Old remote 'origin' saved: " + oldRemoteUrl);
		}
		else {
			log("No existing remote 'origin' found, nothing to save.");
		}
	}

	int deleteResult = git_remote_delete(repo, "origin");
	log("git_remote_delete result: " + to_string(deleteResult) +
		(deleteResult == 0 ? " (removed)" : " (not found or already absent)"));

	git_remote* remote = nullptr;
	if (git_remote_create(&remote, repo, "origin", repoDestination.c_str()) != 0) {
		printGitError("Unable to create the remote");
		return exitWithError("git_remote_create failed", nullptr, repo);
	}

	const char* remoteUrl = git_remote_url(remote);
	log("Remote 'origin' URL verified: " + string(remoteUrl ? remoteUrl : "null"));

	if (!remoteUrl || string(remoteUrl) != repoDestination) {
		return exitWithError("Remote URL mismatch! Expected: " + repoDestination +
			" Got: " + (remoteUrl ? remoteUrl : "null"), remote, repo);
	}

	log("\n - Step 3: Detecting the default branch of the remote");

	git_remote_callbacks callbacks = GIT_REMOTE_CALLBACKS_INIT;
	callbacks.credentials = credentialsCallback;

	string remoteDefaultBranch;

	int connectResult = git_remote_connect(remote, GIT_DIRECTION_FETCH, &callbacks, nullptr, nullptr);
	log("git_remote_connect result: " + to_string(connectResult));

	if (connectResult != 0) {
		printGitError("Unable to connect to remote");
		return exitWithError("git_remote_connect failed", remote, repo);
	}

	const git_remote_head** remoteHeads = nullptr;
	size_t remoteHeadsCount = 0;

	if (git_remote_ls(&remoteHeads, &remoteHeadsCount, remote) == 0) {
		log("Remote heads count: " + to_string(remoteHeadsCount));
		for (size_t i = 0; i < remoteHeadsCount; i++) {
			if (remoteHeads[i]->symref_target != nullptr) {
				string symref = remoteHeads[i]->symref_target;
				const string prefix = "refs/heads/";
				if (symref.find(prefix) == 0) {
					remoteDefaultBranch = symref.substr(prefix.length());
					log("Remote default branch detected via symref: " + remoteDefaultBranch);
					break;
				}
			}
		}
	}

	git_remote_disconnect(remote);
	log("Remote disconnected.");

	if (remoteDefaultBranch.empty()) {
		log("Remote branch not detected, using local: " + localMainBranch);
		remoteDefaultBranch = localMainBranch;
	}

	log("Main branch of the remote: " + remoteDefaultBranch);

	log("\n - Step 4: Main branch alignment");

	if (localMainBranch != remoteDefaultBranch) {
		log("Renaming '" + localMainBranch + "' to '" + remoteDefaultBranch + "'...");

		git_reference* branchRef = nullptr;
		if (git_branch_lookup(&branchRef, repo, localMainBranch.c_str(), GIT_BRANCH_LOCAL) != 0) {
			printGitError("404 Branch not found");
			return exitWithError("git_branch_lookup failed", remote, repo);
		}

		git_reference* newBranchRef = nullptr;
		if (git_branch_move(&newBranchRef, branchRef, remoteDefaultBranch.c_str(), 0) != 0) {
			printGitError("Renaming failed");
			git_reference_free(branchRef);
			return exitWithError("git_branch_move failed", remote, repo);
		}

		git_reference_free(branchRef);
		git_reference_free(newBranchRef);
		localMainBranch = remoteDefaultBranch;
		log("Renaming completed.");
	}
	else {
		log("No renaming required.");
	}

	log("\n - Step 5: Push all branches");

	git_branch_iterator* branchIter = nullptr;
	git_branch_iterator_new(&branchIter, repo, GIT_BRANCH_LOCAL);

	git_strarray refspecs = {};
	vector<string> refspecStrings;
	vector<const char*> refspecPtrs;

	git_reference* branchRef = nullptr;
	git_branch_t branchType;

	while (git_branch_next(&branchRef, &branchType, branchIter) == 0) {
		const char* branchName = nullptr;
		git_branch_name(&branchName, branchRef);

		git_object* obj = nullptr;
		string refPath = string("refs/heads/") + branchName;
		if (git_revparse_single(&obj, repo, refPath.c_str()) != 0) {
			log("Skipping empty branch (no commits): " + string(branchName));
			git_reference_free(branchRef);
			continue;
		}
		git_object_free(obj);

		string refspec = refPath + ":" + refPath;
		refspecStrings.push_back(refspec);
		log("Branch queued for push: " + string(branchName));
		git_reference_free(branchRef);
	}
	git_branch_iterator_free(branchIter);

	if (refspecStrings.empty()) {
		return exitWithError("No branches with commits found to push.", remote, repo);
	}

	for (auto& s : refspecStrings) refspecPtrs.push_back(s.c_str());
	refspecs.strings = const_cast<char**>(refspecPtrs.data());
	refspecs.count = refspecPtrs.size();

	git_push_options pushOpts = GIT_PUSH_OPTIONS_INIT;
	pushOpts.callbacks.credentials = credentialsCallback;

	log("Pushing " + to_string(refspecs.count) + " branch(es)...");
	int pushResult = git_remote_push(remote, &refspecs, &pushOpts);
	log("git_remote_push result: " + to_string(pushResult));

	if (pushResult != 0) {
		printGitError("Push failed");
		log("The remote repo may have an existing or divergent history.");

		cout << "\nDo you want to force the push? This will permanently overwrite the remote history." << endl;
		cout << "[Y/n]: ";
		string confirmation;
		getline(cin, confirmation);
		log("User confirmation: " + confirmation);

		if (confirmation != "Y" && confirmation != "y") {
			return exitWithError("Operation canceled by user.", remote, repo);
		}

		vector<string> forceRefspecStrings;
		vector<const char*> forceRefspecPtrs;
		for (auto& s : refspecStrings) forceRefspecStrings.push_back("+" + s);
		for (auto& s : forceRefspecStrings) forceRefspecPtrs.push_back(s.c_str());

		git_strarray forceRefspecs = {};
		forceRefspecs.strings = const_cast<char**>(forceRefspecPtrs.data());
		forceRefspecs.count = forceRefspecPtrs.size();

		log("Force push in progress...");
		int forceResult = git_remote_push(remote, &forceRefspecs, &pushOpts);
		log("git_remote_push (force) result: " + to_string(forceResult));

		if (forceResult != 0) {
			printGitError("Force push failed");
			return exitWithError("Force push failed.", remote, repo);
		}
	}

	log("\n - Step 6: Tag push");

	const char* tagRefspecStr = "refs/tags/*:refs/tags/*";
	git_strarray tagRefspecs = {};
	tagRefspecs.strings = const_cast<char**>(&tagRefspecStr);
	tagRefspecs.count = 1;

	int tagResult = git_remote_push(remote, &tagRefspecs, &pushOpts);
	log("git_remote_push (tags) result: " + to_string(tagResult));
	if (tagResult != 0) {
		printGitError("Warning: tag push failed (non-blocking)");
	}

	log("\n - Restoring original remote configuration");

	git_remote_free(remote);
	remote = nullptr;

	git_remote_delete(repo, "origin");

	if (!oldRemoteUrl.empty()) {
		git_remote* restoredRemote = nullptr;
		if (git_remote_create(&restoredRemote, repo, "origin", oldRemoteUrl.c_str()) == 0) {
			git_remote_free(restoredRemote);
			log("Remote 'origin' restored to: " + oldRemoteUrl);
		}
		else {
			printGitError("Warning: failed to restore old remote (non-blocking)");
		}
	}
	else {
		log("No previous remote to restore.");
	}

	// Cleanup
	git_repository_free(repo);
	git_libgit2_shutdown();

	log("\n - Transfer completed successfully!");
	log("New Repo     : " + repoDestination);
	log("Main Branch  : " + localMainBranch);
	log("Log saved to : git_transfer.log");

	if (logFile.is_open()) logFile.close();

	return 0;
}