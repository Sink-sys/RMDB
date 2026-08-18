create table grade (course char(20),id int,score float);
insert into grade values('DataStructure',1,95);
insert into grade values('DataStructure',2,93.5);
insert into grade values('DataStructure',3,94.5);
insert into grade values('ComputerNetworks',1,99);
insert into grade values('ComputerNetworks',2,88.5);
insert into grade values('ComputerNetworks',3,92.5);
select id , score from grade group by course;
select id, MAX(score) as max_score where MAX(score) > 90 from grade group by id;
