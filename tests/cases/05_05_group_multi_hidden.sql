create table grade2 (course char(8), id int, score float);
insert into grade2 values ('DB', 1, 90.0);
insert into grade2 values ('DB', 1, 95.0);
insert into grade2 values ('DB', 2, 80.0);
insert into grade2 values ('OS', 1, 70.0);
insert into grade2 values ('OS', 1, 75.0);
insert into grade2 values ('OS', 2, 88.0);
select course, id, count(*) as cnt, avg(score) as avg_score from grade2 group by course, id having count(*) > 1 order by course asc, id desc limit 3;
