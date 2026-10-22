-- Driven through the real SQLite shell by integration.sqlite_script (tests/integration/CMakeLists.txt):
--   sqlite3.exe -batch C:\Users\User\smoke.db  < smoke.sql
-- The expected output lines are gated there; keep the two in step.
create table fruit(id integer primary key, name text not null, qty integer, price real);
insert into fruit(name, qty, price) values ('apple', 3, 0.5);
insert into fruit(name, qty, price) values ('pear', 5, 0.75);
insert into fruit(name, qty, price) values ('fig', 2, 2.0);
insert into fruit(name, qty, price) values ('plum', 7, 0.25);
select name, qty from fruit order by qty desc;
select count(*), sum(qty), max(price) from fruit;
select name, round(qty * price, 2) as total from fruit where qty > 2 order by total desc, name;
create index fruit_name on fruit(name);
select 'indexed', count(*) from fruit where name = 'pear';
.dump
select 'date', length(date('now')), date('now') glob '[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]', cast(strftime('%Y', 'now') as integer) >= 2024;
pragma integrity_check;
.exit 42
