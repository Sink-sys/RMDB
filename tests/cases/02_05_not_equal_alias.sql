create table not_equal_probe (id int, y float);
insert into not_equal_probe values (1, 0.0);
insert into not_equal_probe values (2, 0.0);
insert into not_equal_probe values (3, 0.0);
select count(*) as nonzero_bang from not_equal_probe where y != 0;
select count(*) as nonzero_angle from not_equal_probe where y <> 0;
select count(*) as zero_count from not_equal_probe where y = 0;
select sum(y) as total_y from not_equal_probe;
