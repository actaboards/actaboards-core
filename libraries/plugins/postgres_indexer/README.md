# postgres_indexer

Zunifikowany plugin PostgreSQL zastepujacy trzy oddzielne pluginy:
- `elasticsearch` - historia operacji kont
- `es_objects` - obiekty blockchain (accounts, assets, balances, proposals, limit orders, bitassets)
- `postgres_content` - content cards i permissions

Wszystkie dane indeksowane sa w jednej bazie PostgreSQL, eliminujac zaleznosc od Elasticsearch i CURL.

## Wymagania

- PostgreSQL >= 12
- libpq (klient PostgreSQL)

## Konfiguracja

### Flagi uruchomieniowe

| Flaga | Typ | Domyslnie | Opis |
|-------|-----|-----------|------|
| `--postgres-indexer-url` | string | (wymagany) | URL polaczenia PostgreSQL, np. `postgresql://user:pass@localhost:5432/dbname` |
| `--postgres-indexer-bulk-replay` | uint32 | 10000 | Rozmiar batcha SQL podczas replay blockchain |
| `--postgres-indexer-bulk-sync` | uint32 | 100 | Rozmiar batcha SQL podczas synchronizacji |
| `--postgres-indexer-visitor` | bool | false | Indeksuj dodatkowe dane: fee, transfer, fill order (spowalnia replay) |
| `--postgres-indexer-operation-object` | bool | true | Zapisuj operacje jako JSONB |
| `--postgres-indexer-operation-string` | bool | false | Zapisuj operacje jako string (wymagane dla trybu query) |
| `--postgres-indexer-start-after-block` | uint32 | 0 | Rozpocznij indeksowanie po bloku N |
| `--postgres-indexer-mode` | uint16 | 0 | Tryb: 0=only_save, 1=only_query, 2=all |
| `--postgres-indexer-proposals` | bool | true | Indeksuj obiekty proposal |
| `--postgres-indexer-accounts` | bool | true | Indeksuj obiekty account |
| `--postgres-indexer-assets` | bool | true | Indeksuj obiekty asset |
| `--postgres-indexer-balances` | bool | true | Indeksuj obiekty balance |
| `--postgres-indexer-limit-orders` | bool | false | Indeksuj obiekty limit order |
| `--postgres-indexer-bitassets` | bool | true | Indeksuj dane bitasset (feed) |
| `--postgres-indexer-keep-only-current` | bool | true | Zachowuj tylko aktualny stan obiektow (UPSERT). Gdy false, kazda zmiana tworzy nowy wiersz. |
| `--postgres-indexer-content-start-block` | uint32 | 0 | Rozpocznij indeksowanie content cards/permissions od bloku N |

### Tryby pracy (--postgres-indexer-mode)

- **0 (only_save)** - Tylko zapis do PostgreSQL. Domyslny tryb.
- **1 (only_query)** - Tylko odczyt z PostgreSQL (serwowanie history API). Wymaga wczesniej zaindeksowanych danych.
- **2 (all)** - Zapis i odczyt. Wymaga `--postgres-indexer-operation-string=true`.

### Przyklad uruchomienia

```bash
witness_node \
  --plugins="witness postgres_indexer market_history grouped_orders api_helper_indexes" \
  --postgres-indexer-url="postgresql://acta:secret@localhost:5432/actaboards" \
  --postgres-indexer-mode=2 \
  --postgres-indexer-operation-string=true \
  --postgres-indexer-visitor=true
```

### Przyklad w config.ini

```ini
plugins = witness postgres_indexer market_history grouped_orders api_helper_indexes
postgres-indexer-url = postgresql://acta:secret@localhost:5432/actaboards
postgres-indexer-mode = 2
postgres-indexer-operation-string = true
postgres-indexer-visitor = true
postgres-indexer-bulk-replay = 10000
postgres-indexer-bulk-sync = 100
```

## Konflikty pluginow

Plugin `postgres_indexer` **nie moze** dzialac jednoczesnie z:
- `account_history`
- `elasticsearch`

Przy probie uruchomienia obu, witness_node zakonczy sie bledem.

## Uwagi dotyczace autentykacji PostgreSQL

Autentykacja jest czescia URL polaczenia (`--postgres-indexer-url`):
```
postgresql://uzytkownik:haslo@host:port/baza
```

Wspierane sa wszystkie metody autentykacji PostgreSQL (password, md5, scram-sha-256, cert, peer).
Do polaczen SSL dodaj parametry w URL:
```
postgresql://user:pass@host/db?sslmode=require&sslcert=/path/cert&sslkey=/path/key
```

## Schemat bazy danych

Plugin automatycznie tworzy nastepujace tabele:

### indexer_operation_history
Historia operacji kont (zastepuje plugin elasticsearch).

| Kolumna | Typ | Opis |
|---------|-----|------|
| account_id | VARCHAR(32) | ID konta (np. 1.2.123) |
| operation_id | VARCHAR(32) | ID operacji (np. 1.11.456) |
| operation_id_num | BIGINT | Numer instancji operacji (do sortowania) |
| sequence | BIGINT | Numer sekwencyjny per konto |
| op_type | SMALLINT | Typ operacji (which()) |
| op_object | JSONB | Operacja jako obiekt JSON |
| op_string | TEXT | Operacja jako string |
| block_num | BIGINT | Numer bloku |
| block_time | TIMESTAMP | Czas bloku |
| trx_id | VARCHAR(64) | ID transakcji |
| fee_* | various | Dane fee (gdy visitor=true) |
| transfer_* | various | Dane transfer (gdy visitor=true) |
| fill_* | various | Dane fill order (gdy visitor=true) |

### indexer_accounts, indexer_assets, indexer_balances, indexer_proposals, indexer_limit_orders, indexer_bitassets
Obiekty blockchain (zastepuja plugin es_objects).

| Kolumna | Typ | Opis |
|---------|-----|------|
| object_id | VARCHAR(32) | ID obiektu blockchain |
| data | JSONB | Pelne dane obiektu |
| block_num | BIGINT | Numer bloku ostatniej modyfikacji |
| block_time | TIMESTAMP | Czas bloku |

Gdy `keep-only-current=true` (domyslnie), tabele maja UNIQUE constraint na `object_id` i uzywaja UPSERT.
Gdy `keep-only-current=false`, kazda zmiana obiektu tworzy nowy wiersz (pelna historia zmian).

### indexer_content_cards
Content cards (zastepuje plugin postgres_content).

| Kolumna | Typ | Opis |
|---------|-----|------|
| content_card_id | VARCHAR(32) | ID content card |
| subject_account | VARCHAR(32) | Konto wlasciciela |
| hash | VARCHAR(256) | Hash dokumentu |
| url | TEXT | URL do pliku |
| type | VARCHAR(64) | Typ dokumentu |
| description | TEXT | Opis |
| content_key | TEXT | Klucz szyfrowania |
| storage_data | TEXT | Dane storage |
| is_removed | BOOLEAN | Flaga usuniecia (soft-delete) |

### indexer_permissions
Permissions (zastepuje plugin postgres_content).

| Kolumna | Typ | Opis |
|---------|-----|------|
| permission_id | VARCHAR(32) | ID uprawnienia |
| subject_account | VARCHAR(32) | Konto wlasciciela |
| operator_account | VARCHAR(32) | Konto operatora |
| permission_type | VARCHAR(64) | Typ uprawnienia |
| object_id | VARCHAR(32) | ID obiektu (content card) |
| content_key | TEXT | Klucz szyfrowania |
| is_removed | BOOLEAN | Flaga usuniecia (soft-delete) |

### indexer_sync_state
Tabela bookkeeping do sledzenia stanu synchronizacji.

## Przyklady zapytan SQL

```sql
-- Historia operacji konta
SELECT * FROM indexer_operation_history
WHERE account_id = '1.2.123'
ORDER BY operation_id_num DESC
LIMIT 20;

-- Operacje transferu z danymi visitor
SELECT account_id, transfer_from, transfer_to, transfer_asset_name, transfer_amount_units, block_time
FROM indexer_operation_history
WHERE op_type = 0 AND transfer_amount > 0
ORDER BY block_time DESC;

-- Aktualne dane konta
SELECT object_id, data->>'name' AS name, data->>'options' AS options
FROM indexer_accounts
WHERE object_id = '1.2.123';

-- Wyszukiwanie kont po nazwie
SELECT object_id, data->>'name' AS name
FROM indexer_accounts
WHERE data->>'name' ILIKE '%alice%';

-- Wszystkie assety
SELECT object_id, data->>'symbol' AS symbol, data->'options'->>'max_supply' AS max_supply
FROM indexer_assets;

-- Content cards uzytkownika
SELECT * FROM indexer_content_cards
WHERE subject_account = '1.2.123' AND is_removed = FALSE
ORDER BY block_time DESC;

-- Kto ma dostep do dokumentu
SELECT p.*, c.description
FROM indexer_permissions p
JOIN indexer_content_cards c ON c.content_card_id = p.object_id
WHERE p.object_id = '1.20.123' AND p.is_removed = FALSE;

-- Dokumenty dostepne dla uzytkownika
SELECT c.*
FROM indexer_content_cards c
JOIN indexer_permissions p ON p.object_id = c.content_card_id
WHERE p.operator_account = '1.2.456'
  AND p.is_removed = FALSE
  AND c.is_removed = FALSE;
```

## Roznice wzgledem oryginalnych pluginow

1. **Brak adaptor_struct** - PostgreSQL JSONB nie wymaga rename'owania pol (w przeciwienstwie do Elasticsearch). Obiekty sa serializowane bezposrednio przez `fc::to_variant()`, co oznacza ze pola takie jak `owner`, `memo`, `proposed_ops` zachowuja oryginalna strukture zamiast byc zmieniane na `owner_`, `memo_` itd.

2. **Bulk operations** - Zamiast Elasticsearch Bulk API, plugin uzywa transakcji PostgreSQL (`BEGIN;...COMMIT;`) z konfigurowalnym rozmiarem batcha.

3. **Jedno polaczenie** - Zamiast trzech oddzielnych polaczen (2x CURL do ES + 1x libpq do PG), plugin uzywa jednego polaczenia libpq.

4. **Brak index-prefix** - Tabele maja stale nazwy (np. `indexer_accounts`), w przeciwienstwie do ES gdzie mozna bylo konfigurowasc prefix indeksow.
