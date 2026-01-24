# PostgreSQL Content Plugin

Plugin do indeksowania operacji `content_cards` i `permissions` w PostgreSQL.

## Wymagania

- PostgreSQL 10+
- libpq-dev (zainstalowane w Dockerfile)

## Konfiguracja

```bash
witness_node --plugins="postgres_content" \
  --postgres-content-url="postgresql://user:password@localhost:5432/dbname" \
  --postgres-content-start-block=0
```

### Opcje

| Opcja | Opis | Domyślnie |
|-------|------|-----------|
| `--postgres-content-url` | Connection string PostgreSQL | (wymagane) |
| `--postgres-content-start-block` | Indeksuj od tego bloku | 0 |

## Schemat bazy danych

Plugin automatycznie tworzy tabele przy starcie.

### Tabela `content_cards`

```sql
SELECT * FROM content_cards WHERE subject_account = '1.2.123' AND is_removed = FALSE;
```

| Kolumna | Typ | Opis |
|---------|-----|------|
| content_card_id | VARCHAR(32) | ID karty (np. 1.20.123) |
| subject_account | VARCHAR(32) | Właściciel |
| hash | VARCHAR(256) | Hash zawartości |
| url | TEXT | URL do zawartości |
| type | VARCHAR(64) | Typ karty |
| description | TEXT | Opis |
| content_key | TEXT | Klucz szyfrowania |
| storage_data | TEXT | Dane storage |
| block_num | BIGINT | Numer bloku |
| block_time | TIMESTAMP | Czas bloku |
| operation_type | SMALLINT | 41=create, 42=update, 43=remove |
| is_removed | BOOLEAN | Czy usunięta |

### Tabela `permissions`

```sql
SELECT * FROM permissions WHERE object_id = '1.20.123' AND is_removed = FALSE;
```

| Kolumna | Typ | Opis |
|---------|-----|------|
| permission_id | VARCHAR(32) | ID uprawnienia (np. 1.21.456) |
| subject_account | VARCHAR(32) | Właściciel zasobu |
| operator_account | VARCHAR(32) | Kto ma dostęp |
| permission_type | VARCHAR(64) | Typ uprawnienia |
| object_id | VARCHAR(32) | ID obiektu (content_card) |
| content_key | TEXT | Klucz dostępu |
| block_num | BIGINT | Numer bloku |
| block_time | TIMESTAMP | Czas bloku |
| operation_type | SMALLINT | 44=create, 45=remove |
| is_removed | BOOLEAN | Czy usunięte |

## Przykładowe zapytania

```sql
-- Karty użytkownika posortowane po dacie
SELECT * FROM content_cards
WHERE subject_account = '1.2.123' AND is_removed = FALSE
ORDER BY block_time DESC;

-- Wyszukiwanie po opisie
SELECT * FROM content_cards
WHERE description ILIKE '%szukana fraza%' AND is_removed = FALSE;

-- Kto ma dostęp do dokumentu
SELECT p.*, c.description
FROM permissions p
JOIN content_cards c ON c.content_card_id = p.object_id
WHERE p.object_id = '1.20.123' AND p.is_removed = FALSE;

-- Dokumenty do których użytkownik ma dostęp
SELECT c.*
FROM content_cards c
JOIN permissions p ON p.object_id = c.content_card_id
WHERE p.operator_account = '1.2.456'
  AND p.is_removed = FALSE
  AND c.is_removed = FALSE;
```

## Docker

```yaml
services:
  postgres:
    image: postgres:15
    environment:
      POSTGRES_DB: actaboards
      POSTGRES_USER: actaboards
      POSTGRES_PASSWORD: secret
    ports:
      - "5432:5432"

  witness:
    build: .
    command: >
      witness_node
      --plugins="postgres_content"
      --postgres-content-url="postgresql://actaboards:secret@postgres:5432/actaboards"
    depends_on:
      - postgres
```
