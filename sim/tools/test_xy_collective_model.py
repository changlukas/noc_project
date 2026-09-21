import pytest
from xy_collective_model import fit, estimate
from run_xy_collectives import cases


def test_identifiable_direction_fit():
    samples=[(q,h,11+3*h+q/8) for q,h in [(64,1),(4096,1),(65536,1),(64,2),(65536,2)]]
    result=fit(samples)
    assert result["completion_overhead_cycles"]==pytest.approx(11)
    assert result["hop_cycles"]==pytest.approx(3)
    assert result["service_bytes_per_cycle"]==pytest.approx(8)


def parameters():
    return dict(driver_issue_cycles=2,directions={a:dict(completion_overhead_cycles=10,
                hop_cycles=2,service_bytes_per_cycle=8) for a in ("east","west","north","south")})


def contract(jobs):
    return dict(costs=dict(transfers=jobs))


def job(issuer,path,count=1,prerequisites=()):
    return dict(issuer=issuer,job=count,path=path,bytes=80,prerequisites=prerequisites)


def test_dependencies_use_completion_not_service_end():
    one=job(0,[0,1])
    two=job(1,[1,2],prerequisites=[(0,1)])
    assert estimate(contract([one]),parameters())==22
    assert estimate(contract([one,two]),parameters())==44


def test_directed_links_and_duplex_resources():
    pairs=contract([job(0,[0,1]),job(1,[1,0])])
    assert estimate(pairs,parameters())==22
    assert estimate(pairs,parameters(),"shared_send_receive")==32
    conflict=contract([job(0,[0,1,2]),job(1,[1,2,3])])
    assert estimate(conflict,parameters())==34


def test_reject_missing_dependencies():
    with pytest.raises(ValueError,match="dependencies"):
        estimate(contract([job(0,[0,1],prerequisites=[(2,1)])]),parameters())


def test_matrix_has_unique_complete_cases():
    matrix=cases()
    assert len(matrix)==74
    assert len({c["tag"] for c in matrix})==74
    assert sum(c["category"]=="measurement" for c in matrix)==30
    assert all(c["backpressure"]==0 for c in matrix if c["category"]=="measurement")
