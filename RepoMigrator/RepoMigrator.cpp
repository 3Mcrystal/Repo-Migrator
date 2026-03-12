#include <iostream>
#include <string>
#include <cstdlib>
#include <filesystem>
#include <algorithm>

using namespace std;
namespace fs = std::filesystem;

string readFileLine(const string& path) {
    FILE* file = nullptr;
    string line;
    if (fopen_s(&file, path.c_str(), "r") == 0 && file) {
        char buffer[256];
        if (fgets(buffer, sizeof(buffer), file)) {
            line = buffer;
            line.erase(remove(line.begin(), line.end(), '\n'), line.end());
            line.erase(remove(line.begin(), line.end(), '\r'), line.end());
        }
        fclose(file);
    }
    return line;
}

// Vérifie si une branche existe localement
bool branchExists(const string& branch) {
    string cmd = "git rev-parse --verify " + branch + " >nul 2>&1";
    return system(cmd.c_str()) == 0;
}

int main() {
    string repoSource;
    string repoDestination;

    cout << "Entrez le chemin complet du depot source (local) : ";
    getline(cin, repoSource);

    cout << "Entrez l'URL (.git) du repo de destination (!VIDE!) : ";
    getline(cin, repoDestination);

    if (!fs::exists(repoSource)) {
        cerr << "Le dossier source n'existe pas : " << repoSource << endl;
        return 1;
    }

    fs::current_path(repoSource);
    cout << "Repertoire courant : " << fs::current_path() << endl;

    cout << "\n=== Étape 1 : Détection de la branche principale locale ===" << endl;

    string localMainBranch;
    if (branchExists("main")) {
        localMainBranch = "main";
    }
    else if (branchExists("master")) {
        localMainBranch = "master";
    }
    else {
        // Fallback : on prend la branche courante
        system("git symbolic-ref --short HEAD > current_branch.txt");
        localMainBranch = readFileLine("current_branch.txt");
        remove("current_branch.txt");
        cout << "Aucune branche main/master trouvée, utilisation de la branche courante." << endl;
    }

    if (localMainBranch.empty()) {
        cerr << "Impossible de détecter une branche principale locale." << endl;
        return 1;
    }

    cout << "Branche principale locale : " << localMainBranch << endl;

    cout << "\n=== Étape 2 : Configuration du remote ===" << endl;

    system("git remote remove origin >nul 2>&1");

    string addRemote = "git remote add origin " + repoDestination;
    system(addRemote.c_str());
    cout << "Remote 'origin' configuré vers : " << repoDestination << endl;

    cout << "\n=== Étape 3 : Détection de la branche par défaut du remote ===" << endl;

    string cmdHead = "git ls-remote --symref " + repoDestination + " HEAD > remote_head.txt";
    system(cmdHead.c_str());

    string remoteDefaultBranch;
    {
        FILE* file = nullptr;
        if (fopen_s(&file, "remote_head.txt", "r") == 0 && file) {
            char buffer[256];
            while (fgets(buffer, sizeof(buffer), file)) {
                string line = buffer;
                if (line.rfind("ref:", 0) == 0) {
                    size_t pos = line.find("refs/heads/");
                    if (pos != string::npos) {
                        remoteDefaultBranch = line.substr(pos + 11);
                        remoteDefaultBranch.erase(remove(remoteDefaultBranch.begin(), remoteDefaultBranch.end(), '\n'), remoteDefaultBranch.end());
                        remoteDefaultBranch.erase(remove(remoteDefaultBranch.begin(), remoteDefaultBranch.end(), '\r'), remoteDefaultBranch.end());
                        break;
                    }
                }
            }
            fclose(file);
        }
    }
    remove("remote_head.txt");

    if (remoteDefaultBranch.empty()) {
        cout << "Branche remote non détectée, utilisation de '" << localMainBranch << "'." << endl;
        remoteDefaultBranch = localMainBranch;
    }

    cout << "Branche principale du remote : " << remoteDefaultBranch << endl;

    cout << "\n=== Étape 4 : Alignement de la branche principale ===" << endl;

    if (localMainBranch != remoteDefaultBranch) {
        cout << "Renommage de '" << localMainBranch << "' en '" << remoteDefaultBranch << "'..." << endl;

        string renameCmd = "git branch -m " + localMainBranch + " " + remoteDefaultBranch;
        if (system(renameCmd.c_str()) != 0) {
            cerr << "Échec du renommage de la branche." << endl;
            return 1;
        }

        localMainBranch = remoteDefaultBranch;
    }
    else {
        cout << "Aucun renommage nécessaire." << endl;
    }

    cout << "\n=== Étape 5 : Push de toutes les branches ===" << endl;
    if (system("git push --all origin") != 0) {
        cerr << "\nÉchec du push des branches." << endl;
        cerr << "Le repo distant a peut-être un historique existant ou divergent." << endl;

        cout << "\nVoulez-vous forcer le push ? Cela écrasera définitivement l'historique distant." << endl;
        cout << "[Y/n] ";
        string confirmation;
        getline(cin, confirmation);

        if (confirmation != "Y"|| confirmation != "y") {
            cerr << "Opération annulée." << endl;
            return 1;
        }

        cout << "\nForce push en cours..." << endl;
        if (system("git push --force --all origin") != 0) {
            cerr << "Échec du force push. Vérifiez vos droits sur le repo distant." << endl;
            return 1;
        }
    }

    cout << "\n=== Étape 6 : Push des tags ===" << endl;
    system("git push --tags origin");

    cout << "\n=== Étape 7 : Configuration de l'upstream ===" << endl;
    string upstreamCmd = "git push -u origin " + localMainBranch;
    if (system(upstreamCmd.c_str()) != 0) {
        cerr << "Échec de la configuration de l'upstream." << endl;
        return 1;
    }

    cout << "\n=== Transfert terminé avec succès ! ===" << endl;
    cout << "Nouveau dépôt : " << repoDestination << endl;
    cout << "Branche principale : " << localMainBranch << endl;

    return 0;
}