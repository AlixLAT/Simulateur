// =========================================================
// domoserver.c - Serveur HTTP Windows + SQLite pour Domo-Connect
// Compilation : gcc domoserver.c sqlite3.c -o domoserver.exe -lws2_32 -lsqlite3
// =========================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <sqlite3.h>

#pragma comment(lib, "ws2_32.lib")

#define PORT 8080
#define DB_FILE "etat_appareils.db"
#define RECV_BUF 8192
// Adresses par défaut alignées avec la base de données
#define DEFAULT_SIM_IP "192.168.56.1"      // IP par défaut du simulateur (fallback)
#define DEFAULT_SIM_PORT 60396          // Port par défaut du simulateur (fallback)         


// --- Fonction pour envoyer au simulateur (avec statut de connexion) ---
void envoyer_au_simulateur(const char *ip, int port, const char *type, const char *input, const char *etat) {
    WSADATA wsa;
    SOCKET sock;
    struct sockaddr_in simAddr;
    char ip_sim[16];
    
    // Déterminer l'IP et le port à utiliser
    const char *final_ip = (ip && strlen(ip) > 0) ? ip : DEFAULT_SIM_IP;
    int final_port = (port > 0) ? port : DEFAULT_SIM_PORT;

    WSAStartup(MAKEWORD(2, 2), &wsa);
    sock = socket(AF_INET, SOCK_STREAM, 0);

    simAddr.sin_family = AF_INET;
    simAddr.sin_addr.s_addr = inet_addr(final_ip);
    simAddr.sin_port = htons(final_port);

    // Tentative de connexion
    if (connect(sock, (struct sockaddr *)&simAddr, sizeof(simAddr)) == SOCKET_ERROR) {
        printf("❌ Simulateur non connecté. Impossible de joindre l'appareil (%s:%d).\n", final_ip, final_port);
        closesocket(sock);
        WSACleanup();
        return;
    }

    char message[256];
    // Message au format : type:input:etat
    snprintf(message, sizeof(message), "%s:%s:%s", type, input, etat); 
    send(sock, message, strlen(message), 0);
    
    printf("✅ Simulateur connecté. Commande envoyée à %s:%d : %s\n", final_ip, final_port, message);

    closesocket(sock);
    WSACleanup();
}

// =========================================================
// UTILITAIRES
// =========================================================
void url_decode(char *dst, const char *src) {
    char a, b;
    while (*src) {
        if (*src == '%') {
            a = src[1]; b = src[2];
            if (a && b) {
                int val = (int)strtol((char[]){a, b, 0}, NULL, 16);
                *dst++ = (char)val;
                src += 3;
            } else src++;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else *dst++ = *src++;
    }
    *dst = '\0';
}

void send_file_response(SOCKET sock, const char *filename, const char *extra_message) {
    FILE *f = fopen(filename, "rb");
    if (!f) {
        const char *nf = "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\n\r\n<h1>404 - Page non trouvée</h1>";
        send(sock, nf, (int)strlen(nf), 0);
        return;
    }

    static char buffer[32768];
    size_t n = fread(buffer, 1, sizeof(buffer)-1, f);
    buffer[n] = '\0';
    fclose(f);

    char response[65536];
    // Reste de la fonction send_file_response (non modifiée pour la concision)
    if (extra_message && strlen(extra_message) > 0) {
        char *bodyPos = strstr(buffer, "<body");
        if (bodyPos) {
            char *endTag = strchr(bodyPos, '>');
            if (endTag) {
                endTag++;
                snprintf(response, sizeof(response),
                    "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\n\r\n"
                    "%.*s<p style='color:crimson;font-weight:700;'>%s</p>%s",
                    (int)(endTag - buffer), buffer, extra_message, endTag);
                send(sock, response, (int)strlen(response), 0);
                return;
            }
        }
        snprintf(response, sizeof(response),
                      "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\n\r\n%s"
                      "<p style='color:crimson;'>%s</p>", buffer, extra_message);
        send(sock, response, (int)strlen(response), 0);
    } else {
        snprintf(response, sizeof(response),
                      "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\n\r\n%s",
                      buffer);
        send(sock, response, (int)strlen(response), 0);
    }
}

void send_404_response(SOCKET sock) {
    const char *nf = "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\n\r\n<h1>404 - Page non trouvée</h1>";
    send(sock, nf, (int)strlen(nf), 0);
}
// =========================================================
// DATABASE
// =========================================================
void initDB(sqlite3 *db) {
    char *err = NULL;
    const char *sql =
        "CREATE TABLE IF NOT EXISTS etat_appareils ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "appareil TEXT UNIQUE, "
        "etat TEXT, "
        "ip TEXT, " 
        "input TEXT, " 
        "port INTEGER DEFAULT 49644, " 
        "compteur_on INTEGER DEFAULT 0, "
        "compteur_off INTEGER DEFAULT 0, "
        "dernier_changement DATETIME DEFAULT CURRENT_TIMESTAMP);";
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "Erreur creation table: %s\n", err);
        sqlite3_free(err);
    }
    printf("[DB] Table prête.\n");
}


void resetDB(sqlite3 *db) {
    const char *del = "DELETE FROM etat_appareils;";
    sqlite3_exec(db, del, NULL, NULL, NULL);
    // On ne fait pas initDB ici pour ne pas avoir de conflit avec l'initialisation de main()
    printf("[DB] Base '%s' réinitialisée.\n", DB_FILE);
}

// Fonction pour obtenir tous les détails de l'appareil
void getAppareilDetails(sqlite3 *db, const char *nom, char *ip_out, size_t ip_len, char *input_out, size_t input_len, int *port_out, char *etat_out, size_t etat_len) {
    const char *sql = "SELECT ip, input, port, etat FROM etat_appareils WHERE appareil = ?;";
    sqlite3_stmt *stmt = NULL;
    
    // Valeurs par défaut/Fallback
    strncpy(ip_out, DEFAULT_SIM_IP, ip_len);
    strncpy(input_out, "00000000", input_len);
    *port_out = DEFAULT_SIM_PORT;
    strncpy(etat_out, "OFF", etat_len); 

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, nom, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char *ip_db = sqlite3_column_text(stmt, 0);
            const unsigned char *input_db = sqlite3_column_text(stmt, 1);
            int port_db = sqlite3_column_int(stmt, 2);
            const unsigned char *etat_db = sqlite3_column_text(stmt, 3);
            
            if (ip_db) strncpy(ip_out, (const char*)ip_db, ip_len);
            if (input_db) strncpy(input_out, (const char*)input_db, input_len);
            *port_out = port_db;
            if (etat_db) strncpy(etat_out, (const char*)etat_db, etat_len);
        }
    }
    if (stmt) sqlite3_finalize(stmt);
}


void getEtat(sqlite3 *db, const char *nom, char *etat_out, size_t outlen) {
    char ip[16], input[9], etat[32];
    int port;
    getAppareilDetails(db, nom, ip, sizeof(ip), input, sizeof(input), &port, etat, sizeof(etat));
    strncpy(etat_out, etat, outlen);
}


void majEtat(sqlite3 *db, const char *nom, const char *etat) {
    char actuel[32];
    getEtat(db, nom, actuel, sizeof(actuel));

    // Ajout si l'appareil n'existe pas (pour les appareils "simples" comme 'lumiere')
    if (strcmp(actuel, "OFF") == 0 && strcmp(etat, "OFF") != 0) {
        // Tente une insertion si l'appareil est inconnu et qu'on l'allume.
        // On suppose que les appareils principaux sont déjà insérés.
        char sql_insert[256];
        snprintf(sql_insert, sizeof(sql_insert),
            "INSERT OR IGNORE INTO etat_appareils (appareil, etat, ip, input) VALUES ('%s', '%s', '%s', '%s');",
            nom, etat, DEFAULT_SIM_IP, "00000000"); 
        sqlite3_exec(db, sql_insert, 0, 0, 0);
    }
    
    // Logique de mise à jour et incrémentation des compteurs ON/OFF
    const char *sql = "UPDATE etat_appareils SET etat=?, dernier_changement=CURRENT_TIMESTAMP WHERE appareil=?;";

    if (strcmp(etat, "ON") == 0 && strcmp(actuel, "ON") != 0)
        sql = "UPDATE etat_appareils SET etat=?, dernier_changement=CURRENT_TIMESTAMP, compteur_on = compteur_on + 1 WHERE appareil=?;";
    else if (strcmp(etat, "OFF") == 0 && strcmp(actuel, "OFF") != 0)
        sql = "UPDATE etat_appareils SET etat=?, dernier_changement=CURRENT_TIMESTAMP, compteur_off = compteur_off + 1 WHERE appareil=?;";


    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, etat, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, nom, -1, SQLITE_STATIC);
        sqlite3_step(stmt);
    }
    if (stmt) sqlite3_finalize(stmt);
}

// Fonction d'insertion des données initiales (vos 96 appareils)
void insert_initial_devices(sqlite3 *db) {
    // Format: "Nom de l'appareil", "IP", "Input", "État initial", "Port"
    const char *devices[] = {
        // LAMPS (192.168.0.100)
        "Cuisine - Luminaire entrée", "192.168.0.100", "00000001", "OFF", "49644",
        "Cuisine - Luminaire îlot central", "192.168.0.100", "00000010", "OFF", "49644",
        "Salon - Luminaire salon nord", "192.168.0.100", "00010111", "ON", "49644",
        "Salon - Applique cheminée sud", "192.168.0.100", "00011011", "ON", "49644",
        "Garages nord - Projecteur extérieur entrée véhicule nord", "192.168.0.100", "00111110", "OFF", "49644",
        "Garages nord - Projecteur extérieur entrée véhicule sud", "192.168.0.100", "00111111", "OFF", "49644",
        "Garages ouest - Hublot entrée ouest", "192.168.0.100", "01000011", "OFF", "49644",
        "Terrasse - Hublot porte chambre invités", "192.168.0.100", "01000100", "OFF", "49644",
        "Terrasse - Ensemble de spots immergés piscine", "192.168.0.100", "01000111", "OFF", "49644",
        "Hall sud - Hublot ouest", "192.168.0.100", "01001000", "OFF", "49644",
        "Hall sud - Projecteur extérieur ouest", "192.168.0.100", "01001010", "OFF", "49644",
        "Garages nord - Hublot entrée est", "192.168.0.100", "01000000", "OFF", "49644",
        "Garages ouest - Projecteur extérieur entrée véhicule est", "192.168.0.100", "01000001", "OFF", "49644",
        "Garages ouest - Projecteur extérieur entrée véhicule ouest", "192.168.0.100", "01000010", "OFF", "49644",
        "Terrasse - Hublot porte cuisine", "192.168.0.100", "01000101", "OFF", "49644",
        "Terrasse - Hublot cuisine d'été", "192.168.0.100", "01000110", "OFF", "49644",
        "Hall sud - Hublot est", "192.168.0.100", "01001001", "OFF", "49644",
        "Hall sud - Projecteur extérieur est", "192.168.0.100", "01001011", "OFF", "49644",
        "Cuisine - Plan de travail est (gauche)", "192.168.0.100", "00000011", "OFF", "49644",
        "Cuisine - Plan de travail est (droit)", "192.168.0.100", "00000100", "OFF", "49644",
        "Cuisine - Plan de travail ouest (droite)", "192.168.0.100", "00000101", "OFF", "49644",
        "Cuisine - Plan de travail ouest (gauche)", "192.168.0.100", "00000110", "OFF", "49644",
        "Cuisine - Plan de travail sud", "192.168.0.100", "00000111", "OFF", "49644",
        "Suite parentale - Luminaire central", "192.168.0.100", "00001000", "OFF", "49644",
        "Suite parentale - Applique nord-ouest", "192.168.0.100", "00001001", "OFF", "49644",
        "Suite parentale - Applique nord-est", "192.168.0.100", "00001010", "OFF", "49644",
        "Suite parentale - Applique sud-ouest", "192.168.0.100", "00001011", "OFF", "49644",
        "Suite parentale - Applique sud", "192.168.0.100", "00001100", "OFF", "49644",
        "Suite parentale - Salle de bain et dressing - Luminaire entrée salle de bain", "192.168.0.100", "00001101", "OFF", "49644",
        "Suite parentale - Salle de bain et dressing - Luminaire salle de bain central", "192.168.0.100", "00001110", "OFF", "49644",
        "Suite parentale - Salle de bain et dressing - Baignoire", "192.168.0.100", "00001111", "OFF", "49644",
        "Suite parentale - Salle de bain et dressing - Lavabo nord", "192.168.0.100", "00010000", "OFF", "49644",
        "Suite parentale - Salle de bain et dressing - Lavabo est", "192.168.0.100", "00010001", "OFF", "49644",
        "Suite parentale - Salle de bain et dressing - Luminaire douche", "192.168.0.100", "00010010", "OFF", "49644",
        "Suite parentale - Salle de bain et dressing - Luminaire WC", "192.168.0.100", "00010011", "OFF", "49644",
        "Suite parentale - Salle de bain et dressing - Luminaire nord dressing", "192.168.0.100", "00010100", "OFF", "49644",
        "Suite parentale - Salle de bain et dressing - Luminaire sud dressing", "192.168.0.100", "00010101", "OFF", "49644",
        "Suite parentale - Vestibule - Luminaire vestibule", "192.168.0.100", "00010110", "OFF", "49644",
        "Salon - Luminaire salon sud", "192.168.0.100", "00011000", "ON", "49644",
        "Salon - Applique nord", "192.168.0.100", "00011001", "OFF", "49644",
        "Salon - Applique cheminée nord", "192.168.0.100", "00011010", "ON", "49644",
        "Salon - Applique sud", "192.168.0.100", "00011100", "OFF", "49644",
        "Salon - Applique grand mur nord", "192.168.0.100", "00011101", "ON", "49644",
        "Salon - Applique grand mur sud", "192.168.0.100", "00011110", "ON", "49644",
        "Salle à manger - Luminaire central", "192.168.0.100", "00011111", "OFF", "49644",
        "Salle à manger - Applique nord ouest", "192.168.0.100", "00100000", "OFF", "49644",
        "Salle à manger - Applique nord est", "192.168.0.100", "00100001", "OFF", "49644",
        "Salle à manger - Applique nord mur ouest", "192.168.0.100", "00100010", "OFF", "49644",
        "Salle à manger - Applique sud mur ouest", "192.168.0.100", "00100011", "OFF", "49644",
        "Salle à manger - Applique sud-est", "192.168.0.100", "00100100", "OFF", "49644",
        "Placard - Applique", "192.168.0.100", "00100101", "OFF", "49644",
        "WC - Luminaire central", "192.168.0.100", "00100110", "OFF", "49644",
        "Escalier central - Luminaire central", "192.168.0.100", "00100111", "ON", "49644",
        "Escalier central - Luminaire sud", "192.168.0.100", "00101000", "ON", "49644",
        "Bibliothèque - Luminaire central ouest", "192.168.0.100", "00101001", "ON", "49644",
        "Bibliothèque - Luminaire central est", "192.168.0.100", "00101010", "OFF", "49644",
        "Chambre invités - Luminaire central", "192.168.0.100", "00101011", "OFF", "49644",
        "Chambre invités - Applique ouest", "192.168.0.100", "00101100", "OFF", "49644",
        "Chambre invités - Applique sud-ouest", "192.168.0.100", "00101101", "OFF", "49644",
        "Chambre invités - Applique sud", "192.168.0.100", "00101110", "OFF", "49644",
        "Chambre invités - Salle de bain et dressing - Luminaire central salle de bain", "192.168.0.100", "00101111", "OFF", "49644",
        "Chambre invités - Salle de bain et dressing - Luminaire central dressing", "192.168.0.100", "00110000", "OFF", "49644",
        "Chambre invités - Salle de bain et dressing - Applique sud salle de bain", "192.168.0.100", "00110001", "OFF", "49644",
        "Chambre invités - Salle de bain et dressing - Luminaire central douche", "192.168.0.100", "00110010", "OFF", "49644",
        "Hall nord - Luminaire central", "192.168.0.100", "00110011", "OFF", "49644",
        "Hall nord - Applique ouest", "192.168.0.100", "00110100", "OFF", "49644",
        "Hall nord - Applique est", "192.168.0.100", "00110101", "OFF", "49644",
        "Garages nord - Luminaire central nord-ouest", "192.168.0.100", "00110110", "ON", "49644",
        "Garages nord - Luminaire central nord-est", "192.168.0.100", "00110111", "OFF", "49644",
        "Garages nord - Luminaire central sud-ouest", "192.168.0.100", "00111000", "OFF", "49644",
        "Garages nord - Luminaire central sud-est", "192.168.0.100", "00111001", "OFF", "49644",
        "Garages ouest - Luminaire central nord-ouest", "192.168.0.100", "00111010", "OFF", "49644",
        "Garages ouest - Luminaire central nord-est", "192.168.0.100", "00111011", "OFF", "49644",
        "Garages ouest - Luminaire central sud-ouest", "192.168.0.100", "00111100", "OFF", "49644",
        "Garages ouest - Luminaire central sud-est", "192.168.0.100", "00111101", "OFF", "49644",

        // LAMPS (192.168.0.110)
        "Bureau - Luminaire central", "192.168.0.110", "00000001", "OFF", "49644",
        "Chambre nord est - Luminaire central", "192.168.0.110", "00000010", "OFF", "49644",
        "Chambre nord est - Salle de bain - Luminaire central", "192.168.0.110", "00000011", "ON", "49644",
        "Chambre nord est - Applique nord", "192.168.0.110", "00010001", "ON", "49644",
        "Chambre nord est - Applique sud", "192.168.0.110", "00010010", "ON", "49644",
        "Chambre nord est - Salle de bain - Lavabo ouest", "192.168.0.110", "00010011", "OFF", "49644",
        "Chambre nord est - Salle de bain - Lavabo est", "192.168.0.110", "00010100", "OFF", "49644",
        "Chambre nord est - Salle de bain - Baignoire", "192.168.0.110", "00010101", "OFF", "49644",
        "Escalier est - Luminaire central", "192.168.0.110", "00000100", "OFF", "49644",
        "Escalier est - Applique ouest", "192.168.0.110", "00010110", "OFF", "49644",
        "Escalier est - Applique est", "192.168.0.110", "00010111", "OFF", "49644",
        "Chambre sud est - Luminaire central", "192.168.0.110", "00000101", "OFF", "49644",
        "Chambre sud est - Dressing - Luminaire central", "192.168.0.110", "00000110", "OFF", "49644",
        "Chambre sud est - Salle de bain - Luminaire central", "192.168.0.110", "00000111", "OFF", "49644",
        "Chambre sud est - Applique ouest", "192.168.0.110", "00011000", "OFF", "49644",
        "Chambre sud est - Applique est", "192.168.0.110", "00011001", "OFF", "49644",
        "Chambre sud est - Salle de bain - Lavabo", "192.168.0.110", "00011010", "OFF", "49644",
        "Chambre sud est - Salle de bain - Baignoire", "192.168.0.110", "00011011", "OFF", "49644",
        "Chambre sud ouest - Luminaire central", "192.168.0.110", "00001000", "OFF", "49644",
        "Chambre sud ouest - Dressing - Luminaire central", "192.168.0.110", "00001001", "OFF", "49644",
        "Chambre sud ouest - Salle de bain - Luminaire central", "192.168.0.110", "00001010", "OFF", "49644",
        "Chambre sud ouest - Applique ouest", "192.168.0.110", "00100010", "OFF", "49644",
        "Chambre sud ouest - Applique est", "192.168.0.110", "00100011", "OFF", "49644",
        "Chambre sud ouest - Salle de bain - Lavabo", "192.168.0.110", "00100100", "OFF", "49644",
        "Chambre sud ouest - Salle de bain - Baignoire", "192.168.0.110", "00100101", "OFF", "49644",
        "Salle de jeux - Luminaire central nord", "192.168.0.110", "00001011", "OFF", "49644",
        "Salle de jeux - Luminaire central sud", "192.168.0.110", "00001100", "OFF", "49644",
        "Salle de jeux - Applique nord ouest", "192.168.0.110", "00011100", "OFF", "49644",
        "Salle de jeux - Applique nord est", "192.168.0.110", "00011101", "OFF", "49644",
        "Salle de jeux - Applique ouest", "192.168.0.110", "00011110", "OFF", "49644",
        "Salle de jeux - Applique sud ouest", "192.168.0.110", "00011111", "OFF", "49644",
        "Salle de jeux - Applique sud est", "192.168.0.110", "00100000", "OFF", "49644",
        "Salle de jeux - Applique est", "192.168.0.110", "00100001", "OFF", "49644",
        "Chambre nord ouest - Luminaire central", "192.168.0.110", "00001101", "OFF", "49644",
        "Chambre nord ouest - Dressing - Luminaire central", "192.168.0.110", "00001110", "OFF", "49644",
        "Chambre nord ouest - Salle de bain - Luminaire central", "192.168.0.110", "00001111", "OFF", "49644",
        "Chambre nord ouest - Applique nord", "192.168.0.110", "00101000", "OFF", "49644",
        "Chambre nord ouest - Applique sud", "192.168.0.110", "00101001", "OFF", "49644",
        "Chambre nord ouest - Salle de bain - Lavabo", "192.168.0.110", "00101010", "OFF", "49644",
        "Chambre nord ouest - Salle de bain - Baignoire", "192.168.0.110", "00101011", "OFF", "49644",
        "WC - Lavabo", "192.168.0.110", "00100110", "OFF", "49644",
        "WC - Luminaire central (Etage 2)", "192.168.0.110", "00010000", "OFF", "49644",
        "Accès au grenier - Applique", "192.168.0.110", "00100111", "OFF", "49644",
        "Couloir circulaire - Applique ouest", "192.168.0.110", "00101100", "OFF", "49644",
        "Couloir circulaire - Applique est", "192.168.0.110", "00101101", "OFF", "49644",
        "Couloir circulaire - Applique sud", "192.168.0.110", "00101110", "OFF", "49644",
        "Balcon - Applique ouest", "192.168.0.110", "00101111", "OFF", "49644",
        "Balcon - Applique est", "192.168.0.110", "00110000", "OFF", "49644",

        // STORES (192.168.0.103 et 192.168.0.113)
        "Hall nord - Volet roulant grande baie vitrée", "192.168.0.103", "00000001", "ON", "49644",
        "Garages nord - Porte basculante nord", "192.168.0.103", "00000110", "OFF", "49644",
        "Garages ouest - Porte basculante est", "192.168.0.103", "00001001", "OFF", "49644",
        "Salon - Volet roulant fenêtre sud", "192.168.0.103", "00010111", "ON", "49644",
        "Salle à manger - Volet roulant bow window sud fenêtre est", "192.168.0.103", "00011111", "OFF", "49644",
        "Chambre invités - Volet roulant porte fenêtre terrasse", "192.168.0.103", "00100101", "ON", "49644",
        "Cuisine - Volet roulant porte fenêtre terrasse", "192.168.0.103", "00101001", "ON", "49644",
        "Balcon - Volet roulant fenêtre est", "192.168.0.113", "00000110", "ON", "49644", // IP 192.168.0.113
        "Garages nord - Volet roulant fenêtre est", "192.168.0.103", "00000010", "ON", "49644",
        "Garages nord - Volet roulant fenêtre nord-est", "192.168.0.103", "00000011", "ON", "49644",
        "Garages nord - Volet roulant fenêtre nord", "192.168.0.103", "00000100", "ON", "49644",
        "Garages nord - Volet roulant fenêtre nord-ouest", "192.168.0.103", "00000101", "ON", "49644",
        "Garages nord - Porte basculante sud", "192.168.0.103", "00000111", "OFF", "49644",
        "Garages ouest - Volet roulant fenêtre est", "192.168.0.103", "00001000", "ON", "49644",
        "Garages ouest - Porte basculante ouest", "192.168.0.103", "00001010", "OFF", "49644",
        "Garages ouest - Volet roulant bow window ouest fenêtre nord", "192.168.0.103", "00001011", "ON", "49644",
        "Garages ouest - Volet roulant bow window ouest fenêtre ouest", "192.168.0.103", "00001100", "ON", "49644",
        "Garages ouest - Volet roulant bow window ouest fenêtre sud", "192.168.0.103", "00001101", "ON", "49644",
        "Suite parentale - Volet roulant bow window ouest fenêtre nord", "192.168.0.103", "00001110", "ON", "49644",
        "Suite parentale - Volet roulant bow window ouest fenêtre ouest", "192.168.0.103", "00001111", "ON", "49644",
        "Suite parentale - Volet roulant bow window ouest fenêtre sud", "192.168.0.103", "00010000", "ON", "49644",
        
        // Appareils "simples" de test (pour la route /state)
        "lumiere", DEFAULT_SIM_IP, "00000000", "OFF", "49644",
        "volets", DEFAULT_SIM_IP, "00000000", "OFF", "49644",
        "clim", DEFAULT_SIM_IP, "00000000", "OFF", "49644",
        
        NULL 
    };
    
    char sql[512];
    for (int i = 0; devices[i] != NULL; i += 5) {
        snprintf(sql, sizeof(sql),
            "INSERT OR IGNORE INTO etat_appareils (appareil, ip, input, etat, port) VALUES ('%s', '%s', '%s', '%s', %s);",
            devices[i], devices[i+1], devices[i+2], devices[i+3], devices[i+4]);
        sqlite3_exec(db, sql, 0, 0, 0);
    }
    printf("[DB] Appareils initiaux (96) insérés (si non existants).\n");
}


// =========================================================
// QUERY PARSING
// =========================================================
void extract_query(const char *path, char *device_out, char *etat_out, char *type_out) {
    device_out[0] = etat_out[0] = type_out[0] = '\0';
    const char *q = strchr(path, '?');
    if (!q) return;
    q++;

    char tmp[512];
    strncpy(tmp, q, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    char *token = strtok(tmp, "&");
    while (token) {
        if (strncmp(token, "nom=", 4) == 0)
            url_decode(device_out, token + 4);
        else if (strncmp(token, "etat=", 5) == 0)
            url_decode(etat_out, token + 5);
        else if (strncmp(token, "type=", 5) == 0)
            url_decode(type_out, token + 5);
        token = strtok(NULL, "&");
    }

    printf("DEBUG: nom='%s', etat='%s', type='%s'\n", device_out, etat_out, type_out);
}


// =========================================================
// MAIN
// =========================================================
int main(void) {
    WSADATA wsa;
    SOCKET server_sock, client_sock;
    struct sockaddr_in server_addr;
    int addrlen = sizeof(server_addr);

    sqlite3 *db = NULL;
    if (sqlite3_open(DB_FILE, &db) != SQLITE_OK) {
        fprintf(stderr, "Erreur ouverture DB: %s\n", sqlite3_errmsg(db));
        if (db) sqlite3_close(db);
        return 1;
    }
    sqlite3_busy_timeout(db, 5000);
    initDB(db);
    insert_initial_devices(db); 

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        sqlite3_close(db);
        return 1;
    }

    server_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_sock == INVALID_SOCKET) {
        fprintf(stderr, "socket failed\n");
        WSACleanup();
        sqlite3_close(db);
        return 1;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        fprintf(stderr, "bind failed\n");
        closesocket(server_sock);
        WSACleanup();
        sqlite3_close(db);
        return 1;
    }

    if (listen(server_sock, 10) == SOCKET_ERROR) {
        fprintf(stderr, "listen failed\n");
        closesocket(server_sock);
        WSACleanup();
        sqlite3_close(db);
        return 1;
    }

    printf("🌐 Serveur HTTP Domo-Connect prêt sur http://localhost:%d\n", PORT);

    while (1) {
        client_sock = accept(server_sock, (struct sockaddr*)&server_addr, &addrlen);
        if (client_sock == INVALID_SOCKET) continue;

        char recvbuf[RECV_BUF];
        memset(recvbuf, 0, sizeof(recvbuf));
        int r = recv(client_sock, recvbuf, sizeof(recvbuf) - 1, 0);
        if (r <= 0) {
            closesocket(client_sock);
            continue;
        }

        char method[16] = {0}, path[1024] = {0};
        sscanf(recvbuf, "%15s %1023s", method, path);
        printf("\n--- Requête: %s %s ---\n", method, path);

        // ROUTES DE FICHIERS (avec gestion de /index ou /)
        if (strcmp(method, "GET") == 0 && (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0))
            send_file_response(client_sock, "index.html", NULL);
        else if (strcmp(method, "GET") == 0 && (strcmp(path, "/login") == 0 || strcmp(path, "/login.html") == 0))
            send_file_response(client_sock, "login.html", NULL);
        else if (strcmp(method, "GET") == 0 && (strcmp(path, "/signup") == 0 || strcmp(path, "/signup.html") == 0))
            send_file_response(client_sock, "signup.html", NULL);
        else if (strcmp(method, "GET") == 0 && (strcmp(path, "/accueil") == 0 || strcmp(path, "/accueil.html") == 0))
            send_file_response(client_sock, "accueil.html", NULL);
        else if (strcmp(method, "GET") == 0 && (strcmp(path, "/logout") == 0 || strcmp(path, "/logout.html") == 0))
            send_file_response(client_sock, "logout.html", NULL);

        // ROUTE UPDATE (gestion du changement d'état)
        else if (strcmp(method, "GET") == 0 && strncmp(path, "/update", 7) == 0) {
            char nom[128] = {0}, type[128] = {0}, etat[128] = {0};
            char ip_app[16] = {0}, input_app[9] = {0};
            int port_app = 0;
            char ancien_etat[32] = {0};

            extract_query(path, nom, etat, type);
            printf("[UPDATE] nom=%s | etat=%s | type=%s\n", nom, etat, type);
            if (nom[0] != '\0' && etat[0] != '\0' && type[0] != '\0') {

                // 1. Récupérer les détails IP, Input, Port de la DB
                getAppareilDetails(db, nom, ip_app, sizeof(ip_app), input_app, sizeof(input_app), &port_app, ancien_etat, sizeof(ancien_etat));

                // 2. Mettre à jour l'état dans la base de données
                majEtat(db, nom, etat);

                // 3. Envoyer la commande au simulateur
                envoyer_au_simulateur(ip_app, port_app, type, input_app, etat);

                const char *ok = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nOK";
                send(client_sock, ok, (int)strlen(ok), 0);
            } else {
                const char *bad = "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\n\r\nMissing params";
                send(client_sock, bad, (int)strlen(bad), 0);
            }
        }

        // ROUTE STATE (pour la synchronisation de l'état des appareils de test)
        else if (strcmp(method, "GET") == 0 && strcmp(path, "/state") == 0) {
            char out[1024];
            char e1[32], e2[32], e3[32];
            getEtat(db, "lumiere", e1, sizeof(e1));
            getEtat(db, "volets", e2, sizeof(e2));
            getEtat(db, "clim", e3, sizeof(e3));
            snprintf(out, sizeof(out), "lumiere=%s;volets=%s;clim=%s", e1, e2, e3);
            char resp[2048];
            snprintf(resp, sizeof(resp), "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=UTF-8\r\n\r\n%s", out);
            send(client_sock, resp, (int)strlen(resp), 0);
        }

        // ROUTE RESET DB
        else if (strcmp(method, "GET") == 0 && strcmp(path, "/reset-db") == 0) {
            resetDB(db);
            insert_initial_devices(db); // On réinsère les données après le reset
            const char *ok = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nBase réinitialisée et rechargée";
            send(client_sock, ok, (int)strlen(ok), 0);
        }

        else send_404_response(client_sock);

        closesocket(client_sock);
    }

    closesocket(server_sock);
    sqlite3_close(db);
    WSACleanup();
    return 0;
}