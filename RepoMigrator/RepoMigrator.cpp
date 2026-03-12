#include <iostream>
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

// Displays the current libgit2 error
void printGitError(const string& context) {
	const git_error* e = git_error_last();
	cerr << "[ERREUR] " << context;
	if (e && e->message) {
		cerr << " : " << e->message;
	}
	cerr << endl;
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
// Returns true if found, and fills in username/password
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

// libgit2 authentication callback
// Search in the Windows manager
// If absent, ask the user and save
int credentialsCallback(git_credential** out, const char* url, const char* username_from_url,
	unsigned int allowed_types, void* payload) {

	// URL-based storage key (e.g., “git_transfer:https://github.com/user/repo.git”)
	string target = string("git_transfer:") + url;

	string username, password;

	if (loadCredentials(target, username, password)) {
		cout << "\nCredentials found in Windows Manager for: " << url << endl;
	}
	else {
		cout << "\nAuthentication required for: " << url << endl;

		if (username_from_url && strlen(username_from_url) > 0) {
			username = username_from_url;
			cout << "Username : " << username << endl;
		}
		else {
			cout << "Username : ";
			getline(cin, username);
		}

		cout << "Token / Password: ";
		getline(cin, password);

		saveCredentials(target, username, password);
		cout << "Credentials saved in the Windows manager." << endl;
	}

	return git_credential_userpass_plaintext_new(out, username.c_str(), password.c_str());
}

int main() {
	string repoSource;
	string repoDestination;

	cout << "Enter the full path to the source repository (local) : ";
	getline(cin, repoSource);

	cout << "Enter the URL (.git) of the EMPTY destination repository: ";
	getline(cin, repoDestination);

	if (!fs::exists(repoSource)) {
		cerr << "[ERROR] The source file does not exist : " << repoSource << endl;
		return 1;
	}

	git_libgit2_init();

	git_repository* repo = nullptr;

	cout << "\n - Opening the local repo" << endl;

	if (git_repository_open(&repo, repoSource.c_str()) != 0) {
		printGitError("Unable to open the repository");
		git_libgit2_shutdown();
		return 1;
	}

	cout << "Open repo : " << repoSource << endl;

	cout << "\n - Step 1: Detecting the local main branch" << endl;

	string localMainBranch;

	if (branchExists(repo, "main")) {
		localMainBranch = "main";
	}
	else if (branchExists(repo, "master")) {
		localMainBranch = "master";
	}
	else {
		// Fallback : current branch
		git_reference* headRef = nullptr;
		if (git_repository_head(&headRef, repo) == 0) {
			localMainBranch = git_reference_shorthand(headRef);
			git_reference_free(headRef);
			cout << "No main/master branch, use of the current branch." << endl;
		}
	}

	if (localMainBranch.empty()) {
		cerr << "[ERROR] Unable to detect a local main branch." << endl;
		git_repository_free(repo);
		git_libgit2_shutdown();
		return 1;
	}

	cout << "Local main branch: " << localMainBranch << endl;

	cout << "\n - Step 2: Remote configuration" << endl;

	git_remote_delete(repo, "origin");

	git_remote* remote = nullptr;
	if (git_remote_create(&remote, repo, "origin", repoDestination.c_str()) != 0) {
		printGitError("Unable to create the remote");
		git_repository_free(repo);
		git_libgit2_shutdown();
		return 1;
	}

	cout << "Remote ‘origin’ configured to: " << repoDestination << endl;


	cout << "\n - Step 3: Detecting the default branch of the remote" << endl;

	git_remote_callbacks callbacks = GIT_REMOTE_CALLBACKS_INIT;
	callbacks.credentials = credentialsCallback;

	string remoteDefaultBranch;

	git_remote_connect(remote, GIT_DIRECTION_FETCH, &callbacks, nullptr, nullptr);

	const git_remote_head** remoteHeads = nullptr;
	size_t remoteHeadsCount = 0;

	if (git_remote_ls(&remoteHeads, &remoteHeadsCount, remote) == 0) {
		for (size_t i = 0; i < remoteHeadsCount; i++) {
			string refName = remoteHeads[i]->name;
			// HEAD symref -> refs/heads/xxx
			if (remoteHeads[i]->symref_target != nullptr) {
				string symref = remoteHeads[i]->symref_target;
				const string prefix = "refs/heads/";
				if (symref.find(prefix) == 0) {
					remoteDefaultBranch = symref.substr(prefix.length());
					break;
				}
			}
		}
	}

	git_remote_disconnect(remote);

	if (remoteDefaultBranch.empty()) {
		cout << "Remote branch not detected, using '" << localMainBranch << "'." << endl;
		remoteDefaultBranch = localMainBranch;
	}

	cout << "Main branch of the remote: " << remoteDefaultBranch << endl;

	cout << "\n - Step 4: Main branch alignment" << endl;

	if (localMainBranch != remoteDefaultBranch) {
		cout << "Renaming '" << localMainBranch << "' to '" << remoteDefaultBranch << "'..." << endl;

		git_reference* branchRef = nullptr;
		if (git_branch_lookup(&branchRef, repo, localMainBranch.c_str(), GIT_BRANCH_LOCAL) != 0) {
			printGitError("404 Branch not found");
			git_remote_free(remote);
			git_repository_free(repo);
			git_libgit2_shutdown();
			return 1;
		}

		git_reference* newBranchRef = nullptr;
		if (git_branch_move(&newBranchRef, branchRef, remoteDefaultBranch.c_str(), 0) != 0) {
			printGitError("Renaming failed");
			git_reference_free(branchRef);
			git_remote_free(remote);
			git_repository_free(repo);
			git_libgit2_shutdown();
			return 1;
		}

		git_reference_free(branchRef);
		git_reference_free(newBranchRef);
		localMainBranch = remoteDefaultBranch;
		cout << "Renaming completed." << endl;
	}
	else {
		cout << "No renaming required" << endl;
	}

	cout << "\n - Step 5 : Push all branches" << endl;

	//fetch all local branch
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
		string refspec = string("refs/heads/") + branchName + ":refs/heads/" + branchName;
		refspecStrings.push_back(refspec);
		git_reference_free(branchRef);
	}
	git_branch_iterator_free(branchIter);

	for (auto& s : refspecStrings) refspecPtrs.push_back(s.c_str());
	refspecs.strings = const_cast<char**>(refspecPtrs.data());
	refspecs.count = refspecPtrs.size();

	git_push_options pushOpts = GIT_PUSH_OPTIONS_INIT;
	pushOpts.callbacks.credentials = credentialsCallback;

	int pushResult = git_remote_push(remote, &refspecs, &pushOpts);

	if (pushResult != 0) {
		const git_error* e = git_error_last();
		cerr << "\n[ERROR] Push failed";
		if (e && e->message) cerr << " : " << e->message;
		cerr << endl;
		cerr << "The remote repo may have an existing or divergent history." << endl;

		cout << "\nDo you want to force the push? This will permanently overwrite the remote history." << endl;
		cout << "[Y/n]";
		string confirmation;
		getline(cin, confirmation);

		if (confirmation != "Y" && confirmation != "y") {
			cerr << "Operation canceled" << endl;
			git_remote_free(remote);
			git_repository_free(repo);
			git_libgit2_shutdown();
			return 1;
		}

		vector<string> forceRefspecStrings;
		vector<const char*> forceRefspecPtrs;
		for (auto& s : refspecStrings) forceRefspecStrings.push_back("+" + s);
		for (auto& s : forceRefspecStrings) forceRefspecPtrs.push_back(s.c_str());

		git_strarray forceRefspecs = {};
		forceRefspecs.strings = const_cast<char**>(forceRefspecPtrs.data());
		forceRefspecs.count = forceRefspecPtrs.size();

		cout << "\nForce push in progress..." << endl;
		if (git_remote_push(remote, &forceRefspecs, &pushOpts) != 0) {
			printGitError("Force push failed");
			git_remote_free(remote);
			git_repository_free(repo);
			git_libgit2_shutdown();
			return 1;
		}
	}
	cout << "\n - Step 6 : Tag push" << endl;

	git_strarray tagRefspecs = {};
	const char* tagRefspec = "refs/tags/*:refs/tags/*";
	tagRefspecs.strings = const_cast<char**>(&tagRefspec);
	tagRefspecs.count = 1;

	if (git_remote_push(remote, &tagRefspecs, &pushOpts) != 0) {
		printGitError(" - Warning: tag push failed");
	}

	//cleanup
	git_remote_free(remote);
	git_repository_free(repo);
	git_libgit2_shutdown();

	cout << "\n - Transfer completed successfully!" << endl;
	cout << "New Repo  : " << repoDestination << endl;
	cout << "Main Branch : " << localMainBranch << endl;

	return 0;
}