from dataclasses import replace
import pytest
import dependent_dma_plan as dma
import gen_tb_top
from xy_collective_plan import build, validate, route, CYCLES

@pytest.fixture
def topo():
    return gen_tb_top.load_topology("mesh_4x4_dual_edge_large")

@pytest.mark.parametrize("p", [4,16])
@pytest.mark.parametrize("q", [64,256,16384,262144])
@pytest.mark.parametrize("kind", ["gather","all_gather","reduce_scatter","reduce","all_reduce"])
def test_ownership_and_independent_volume(topo,p,q,kind):
    plan=build(topo,q,"write",f"xy_{kind}_p{p}")
    validate(plan)
    costs=plan.contract["costs"]
    expected={"gather":(p-1)*q, "all_gather":p*(p-1)*q,
              "reduce_scatter":(p-1)*q, "reduce":(p-1)*q+(p-1)*q//p,
              "all_reduce":2*(p-1)*q}[kind]
    assert sum(t.source.length for t in plan.transfers)==expected
    assert max(plan.contract["allocated_bytes"].values()) < 0x2000000
    if kind=="reduce_scatter":
        assert costs["E_byte_hops"]==3*(p-int(p**0.5))*q//2
        assert costs["traffic_depth"]== (3 if p==4 else 6)
        assert costs["L_hops"]==3*(int(p**0.5)-1)
    if kind=="all_gather":
        assert costs["E_byte_hops"]==p*(p-1)*q
        assert costs["N_directed_links"]==p
    assert all(len(route(a,b))==2 for a,b in zip(CYCLES[p],CYCLES[p][1:]+CYCLES[p][:1]))


def test_recursive_intermediate_operand_dependency(topo):
    plan=build(topo,256,"write","xy_reduce_scatter_p16")
    index=next(i for i,t in enumerate(plan.transfers) if "r2" in t.phase)
    transfer=plan.transfers[index]
    assert transfer.local_partial not in plan.initial
    assert len(dma.requirements(transfer))==2
    broken=replace(transfer,prerequisite=None,barrier=())
    with pytest.raises(ValueError,match="dependency"):
        validate(replace(plan,transfers=plan.transfers[:index]+(broken,)+plan.transfers[index+1:]))


def test_wrong_indices_and_missing_outputs(topo):
    plan=build(topo,256,"write","xy_all_reduce_p16")
    transfer=plan.transfers[0]
    with pytest.raises(ValueError,match="identity"):
        validate(replace(plan,transfers=(replace(transfer,destination=replace(transfer.destination,offset=4)),)+plan.transfers[1:]))
    with pytest.raises(ValueError,match="coverage"):
        validate(replace(plan,outputs=plan.outputs[:-1]))


def test_split_waits_for_whole_source_and_keeps_pattern_offsets(topo,tmp_path):
    plan=dma.build_plan(topo,1048576,"write","xy_descriptor_chain")
    assert len(plan.transfers)==4
    assert [t.source.offset for t in plan.transfers]==[0,524288,0,524288]
    assert all(dma.requirements(t)==((0,2),) for t in plan.transfers[2:])
    # The runtime checker covers both the response dependencies and data identity.
    dma.emit(plan,tmp_path,20)
    oracle=(tmp_path/"check_jobs.txt").read_text().splitlines()
    assert all(line.endswith(" 1 0 2") for line in oracle[2:])

@pytest.mark.parametrize("length",[0,1,60,68])
def test_invalid_partition(topo,length):
    with pytest.raises(ValueError):
        build(topo,length,"write","xy_reduce_scatter_p16")


def test_paper_four_node_output_permutation(topo):
    plan=build(topo,64,"write","xy_reduce_scatter_p4")
    # Paper clockwise A,B,C,D = upper left, upper right, lower right, lower left.
    assert {r.node:r.offset//16 for r in plan.outputs}=={4:1,5:2,1:3,0:0}


def test_duplicate_contribution_rejected(topo):
    plan=build(topo,256,"write","xy_reduce_scatter_p4")
    index=4
    t=plan.transfers[index]
    # A prior result for the same destination/index is already in the incoming set.
    candidate=next(r for r in plan.initial if r.node==t.destination.node)
    wrong=replace(candidate,address=candidate.address+t.source.offset,
                  offset=t.source.offset,length=t.source.length)
    # Reusing the same transfer as an additional reduction must fail storage/provenance.
    with pytest.raises(ValueError):
        validate(replace(plan,transfers=plan.transfers[:index]+(replace(t,local_partial=wrong),t)+plan.transfers[index+1:]))


def test_runtime_oracle_snapshot_rejects_changes(topo,tmp_path):
    from xy_runtime_checker import emit,validate_snapshot
    plan=build(topo,64,"write","xy_reduce_scatter_p16")
    oracle=tmp_path/"oracle"
    emit(plan,oracle)
    validate_snapshot(plan,tmp_path)
    path=oracle/"check_jobs.txt"
    path.write_text(path.read_text().replace("reduce_scatter_r1_s0","changed",1))
    with pytest.raises(ValueError,match="oracle provenance"):
        validate_snapshot(plan,tmp_path)


def test_runtime_tb_is_case_independent(topo):
    # The binary reuse key is safe only if all per-case assertions are loaded data.
    small=gen_tb_top.emit_tb_top(topo,True,100,64,"write",True,False,"xy_gather_p4")
    large=gen_tb_top.emit_tb_top(topo,True,100,262144,"write",True,False,"xy_all_reduce_p16")
    assert small==large
    pressured=gen_tb_top.emit_tb_top(topo,True,100,64,"write",True,True,"xy_gather_p4")
    assert pressured!=small


@pytest.mark.parametrize("p", [4,16])
@pytest.mark.parametrize("q", [64,256,16384,262144])
@pytest.mark.parametrize("kind", ["gather","all_gather","reduce_scatter","reduce","all_reduce"])
def test_direct_original_sources_and_costs(topo,p,q,kind):
    plan=build(topo,q,"write",f"xy_direct_{kind}_p{p}")
    nodes=sorted(CYCLES[p])
    root=0 if p==4 else 5
    targets=[root] if kind in ("gather","reduce") else nodes
    size=q//p if kind=="reduce_scatter" else q
    def distance(a,b):
        return abs(a%4-b%4)+abs(a//4-b//4)
    expected_e=sum(distance(a,b)*size for a in nodes for b in targets)
    costs=plan.contract["costs"]
    assert costs["E_byte_hops"]==expected_e
    assert costs["C_bytes"]==(p-1)*size
    assert costs["L_hops"]==max(distance(a,b) for a in nodes for b in targets)
    assert costs["traffic_depth"]==1
    assert costs["N_directed_links"]==((3 if p==4 else 15) if len(targets)==1 else (8 if p==4 else 48))
    assert len(plan.transfers)==(p-1)*(1 if len(targets)==1 else p)
    assert all(not dma.requirements(t) and t.local_partial is None for t in plan.transfers)
    assert all(any(r.node==t.source.node and r.address<=t.source.address and
                   t.source.address+t.source.length<=r.address+r.length for r in plan.initial)
               for t in plan.transfers)
    assert plan.contract["root"]==(root if len(targets)==1 else None)
    assert max(plan.contract["allocated_bytes"].values())<0x2000000
    validate(plan)


@pytest.mark.parametrize("kind",["gather","all_gather","reduce_scatter","reduce","all_reduce"])
def test_direct_missing_duplicate_and_wrong_owner(topo,kind):
    plan=build(topo,256,"write",f"xy_direct_{kind}_p16")
    for outputs in (plan.outputs[:-1], plan.outputs+(plan.outputs[0],),
                    (replace(plan.outputs[0],node=19),)+plan.outputs[1:]):
        with pytest.raises(ValueError,match="direct"):
            validate(replace(plan,outputs=outputs))
    t=plan.transfers[0]
    with pytest.raises(ValueError):
        validate(replace(plan,transfers=(replace(t,source=replace(t.source,shard=99)),)+plan.transfers[1:]))


def test_direct_contribution_source_distinction_and_destination_order(topo):
    plan=build(topo,64,"write","xy_direct_all_reduce_p16")
    assert len({r.shard for r in plan.initial})==16
    for node in range(16):
        assert [t.destination.node for t in plan.transfers if t.issuer==node]==[
            target for target in range(16) if target!=node]
    # The same traffic shape implements AllGather and AllReduce; output semantics differ.
    gather=build(topo,64,"write","xy_direct_all_gather_p16")
    assert plan.transfers==gather.transfers
    assert plan.contract["output_representation"]=="contribution_slots"


def test_direct_matrix_includes_small_gather():
    from run_xy_collectives import cases
    matrix=cases("direct")
    assert "xy_direct_gather_p4" in dma.OPERATIONS
    assert sum(c["category"]=="measurement" for c in matrix)==30
    assert sum(c["category"]=="functional" for c in matrix)==10
    assert sum(c["operation"]=="xy_direct_gather_p4" for c in matrix)==4


def test_direct_extra_unobserved_delivery_rejected(topo):
    plan=build(topo,256,"write","xy_direct_reduce_p16")
    transfer=plan.transfers[0]
    extra=replace(transfer,destination=replace(transfer.destination,
                                             address=transfer.destination.address+0x100000))
    with pytest.raises(ValueError,match="final contribution slots"):
        validate(replace(plan,transfers=plan.transfers+(extra,)))
