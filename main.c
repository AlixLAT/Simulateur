#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include "sqlite3.h"

#define PORT 8080
#define SEUIL_PREDICTION 5  // alerte après 5 OFF

// 🔹 Lecture de l'état d'un appareil
void getEtat(sqlite3 *db, const char *nom, char *etat) {
    const char *sql = "SELECT etat FROM etat_appareils WHERE appareil = ?;";
    sqlite3_stmt *stmt;
    strcpy(etat, "OFF");
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, nom, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char *val = sqlite3_column_text(stmt, 0);
            strcpy(etat, (const char*)val);
        }
        sqlite3_finalize(stmt);
    }
}

// 🔹 Met à jour l'état et incrémente compteur ON/OFF
void majEtat(sqlite3 *db, const char *nom, const char *etat) {
    char etatActuel[8];
    getEtat(db, nom, etatActuel);

    int incrementer_on = strcmp(etatActuel, "ON") != 0 && strcmp(etat, "ON") == 0;
    int incrementer_off = strcmp(etatActuel, "OFF") != 0 && strcmp(etat, "OFF") == 0;

    const char *sql;
    if (incrementer_on) {
        sql = "UPDATE etat_appareils SET etat = ?, dernier_changement = CURRENT_TIMESTAMP, compteur_on = compteur_on + 1 WHERE appareil = ?;";
    } else if (incrementer_off) {
        sql = "UPDATE etat_appareils SET etat = ?, dernier_changement = CURRENT_TIMESTAMP, compteur_off = compteur_off + 1 WHERE appareil = ?;";
    } else {
        sql = "UPDATE etat_appareils SET etat = ?, dernier_changement = CURRENT_TIMESTAMP WHERE appareil = ?;";
    }

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, etat, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, nom, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            printf("❌ Erreur majEtat : %s\n", sqlite3_errmsg(db));
        }
        sqlite3_finalize(stmt);
    }
}

// 🔹 Message prédiction selon compteur_off
void getMessagePrediction(sqlite3 *db, const char *nom, char *message) {
    const char *sql = "SELECT compteur_off FROM etat_appareils WHERE appareil = ?;";
    sqlite3_stmt *stmt;
    strcpy(message, "");
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, nom, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int compteur_off = sqlite3_column_int(stmt, 0);
            if (compteur_off >= SEUIL_PREDICTION) {
                sprintf(message, "⚠️ Attention, %s a été éteint %d fois !", nom, compteur_off);
            }
        }
        sqlite3_finalize(stmt);
    }
}

// 🔹 Réinitialiser compteurs
void resetCompteur(sqlite3 *db, const char *nom) {
    const char *sql = "UPDATE etat_appareils SET compteur_on = 0, compteur_off = 0 WHERE appareil = ?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, nom, -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

// 🔹 Initialisation base
void initDB(sqlite3 *db) {
    char *errMsg = NULL;
    const char *sql =
        "CREATE TABLE IF NOT EXISTS etat_appareils ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "appareil TEXT UNIQUE, "
        "etat TEXT, "
        "compteur_on INTEGER DEFAULT 0, "
        "compteur_off INTEGER DEFAULT 0, "
        "dernier_changement DATETIME DEFAULT CURRENT_TIMESTAMP);";

    if (sqlite3_exec(db, sql, NULL, NULL, &errMsg) != SQLITE_OK) {
        printf("❌ Erreur création table : %s\n", errMsg);
        sqlite3_free(errMsg);
    } else {
        printf("✅ Table prête.\n");
    }

    // Appareils initiaux
    const char *insert1 = "INSERT OR IGNORE INTO etat_appareils (appareil, etat) VALUES ('lumiere','OFF');";
    const char *insert2 = "INSERT OR IGNORE INTO etat_appareils (appareil, etat) VALUES ('volets','OFF');";
    const char *insert3 = "INSERT OR IGNORE INTO etat_appareils (appareil, etat) VALUES ('clim','OFF');";

    sqlite3_exec(db, insert1, NULL, NULL, &errMsg);
    sqlite3_exec(db, insert2, NULL, NULL, &errMsg);
    sqlite3_exec(db, insert3, NULL, NULL, &errMsg);
}

int main() {
    WSADATA wsa;
    SOCKET server_fd, new_socket;
    struct sockaddr_in address;
    int addrlen = sizeof(address);
    char buffer[2048] = {0};
    char response[8192];

    sqlite3 *db;
    if (sqlite3_open("etat_appareils.db", &db) != SQLITE_OK) {
        printf("❌ Erreur ouverture base : %s\n", sqlite3_errmsg(db));
        return 1;
    }
    sqlite3_busy_timeout(db, 5000);
    printf("💾 Base connectée avec succès.\n");

    initDB(db);

    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) return 1;
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET) return 1;

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) == SOCKET_ERROR) return 1;
    if (listen(server_fd, 3) == SOCKET_ERROR) return 1;

    printf("🌐 Serveur domotique prêt sur http://localhost:%d\n", PORT);

    while (1) {
        new_socket = accept(server_fd, (struct sockaddr *)&address, &addrlen);
        if (new_socket == INVALID_SOCKET) continue;

        memset(buffer, 0, sizeof(buffer));
        recv(new_socket, buffer, sizeof(buffer), 0);
        printf("\n🔹 Requête reçue : %s\n", buffer);

        if (strstr(buffer, "favicon.ico")) { closesocket(new_socket); continue; }

        // Actions ON/OFF
        if (strstr(buffer, "lumiere=on")) majEtat(db, "lumiere", "ON");
        if (strstr(buffer, "lumiere=off")) majEtat(db, "lumiere", "OFF");
        if (strstr(buffer, "volets=ouvrir")) majEtat(db, "volets", "ON");
        if (strstr(buffer, "volets=fermer")) majEtat(db, "volets", "OFF");
        if (strstr(buffer, "clim=on")) majEtat(db, "clim", "ON");
        if (strstr(buffer, "clim=off")) majEtat(db, "clim", "OFF");

        // Boutons RESET
        if (strstr(buffer, "lumiere=reset")) resetCompteur(db, "lumiere");
        if (strstr(buffer, "volets=reset")) resetCompteur(db, "volets");
        if (strstr(buffer, "clim=reset")) resetCompteur(db, "clim");

        // Lecture états
        char eLumiere[8], eVolets[8], eClim[8];
        getEtat(db, "lumiere", eLumiere);
        getEtat(db, "volets", eVolets);
        getEtat(db, "clim", eClim);

        // Messages prédiction
        char msgLumiere[128], msgVolets[128], msgClim[128];
        getMessagePrediction(db, "lumiere", msgLumiere);
        getMessagePrediction(db, "volets", msgVolets);
        getMessagePrediction(db, "clim", msgClim);

        // 🔹 Lire fichier HTML
        FILE *f = fopen("login.html", "r");
        if (f) {
            size_t n = fread(response, 1, sizeof(response)-1, f);
            response[n] = 0;
            fclose(f);

            // Copier le HTML dans une page temporaire pour remplacer les placeholders
            char page[8192];
            strcpy(page, response);

            // Remplacer les placeholders
            char *p;

            p = strstr(page, "{{LUMIERE_ETAT}}");
            if (p) snprintf(p, 16, "%s", eLumiere);
            p = strstr(page, "{{LUMIERE_ALERT}}");
            if (p) snprintf(p, 128, "%s", msgLumiere);

            p = strstr(page, "{{VOLETS_ETAT}}");
            if (p) snprintf(p, 16, "%s", eVolets);
            p = strstr(page, "{{VOLETS_ALERT}}");
            if (p) snprintf(p, 128, "%s", msgVolets);

            p = strstr(page, "{{CLIM_ETAT}}");
            if (p) snprintf(p, 16, "%s", eClim);
            p = strstr(page, "{{CLIM_ALERT}}");
            if (p) snprintf(p, 128, "%s", msgClim);

            // 🔹 Envoyer la page finale
            char buffer_final[8192];
            snprintf(buffer_final, sizeof(buffer_final),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: text/html; charset=UTF-8\r\n\r\n%s", page);

            send(new_socket, buffer_final, strlen(buffer_final), 0);
        } else {
            const char *err = "HTTP/1.1 500 Internal Server Error\r\n\r\nErreur: impossible de lire index.html";
            send(new_socket, err, strlen(err), 0);
        }

        closesocket(new_socket);
    }

    closesocket(server_fd);
    sqlite3_close(db);
    WSACleanup();
    return 0;
}
